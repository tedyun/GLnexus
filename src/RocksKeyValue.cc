// Implement a KeyValue interface to a RocksDB on-disk database.
//
#include <cstddef>
#include <map>
#include <iostream>
#include <iomanip>
#include <sstream> // for ostringstream
#include <string>
#include <thread>
#include <unistd.h>
#include "KeyValue.h"
#include "RocksKeyValue.h"
#include "rocksdb/db.h"
#include "rocksdb/slice.h"
#include "rocksdb/options.h"
#include "rocksdb/write_batch.h"
#include "rocksdb/table.h"
#include "rocksdb/memtablerep.h"
#include "rocksdb/cache.h"

namespace GLnexus {
namespace RocksKeyValue {

static size_t totalRAM() {
    // http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system
    static size_t memoized = 0;
    if (!memoized) {
        memoized = (size_t)sysconf( _SC_PHYS_PAGES ) * (size_t)sysconf( _SC_PAGESIZE );
        if (!memoized) {
            memoized = 4<<30;
        }
    }
    return memoized;
}

// Convert a rocksdb Status into a GLnexus Status structure
static Status convertStatus(const rocksdb::Status &s)
{
    rocksdb::Status::Code code = s.code();
    switch (code) {
        // good case
    case rocksdb::Status::kOk: return Status::OK();

        // error cases
    case rocksdb::Status::kNotFound:
        return Status::NotFound();
    case rocksdb::Status::kCorruption:
        return Status::Failure("corruption");
    case rocksdb::Status::kNotSupported:
        return Status::NotImplemented();
    case rocksdb::Status::kInvalidArgument:
        return Status::Invalid("RocksDB kInvalidArgument", s.ToString());
    case rocksdb::Status::kIOError:
        return Status::IOError("RocksDB kIOError", s.ToString());
    case rocksdb::Status::kMergeInProgress:
        return Status::Failure("merge in progress");
    case rocksdb::Status::kIncomplete:
        return Status::Failure("incomplete");
    case rocksdb::Status::kShutdownInProgress:
        return Status::Failure("shutdown in progress");
    case rocksdb::Status::kTimedOut:
        return Status::Failure("timed out");
    case rocksdb::Status::kAborted:
        return Status::Failure("aborted");

        // catch all for unlisted cases, all errors
    default: return Status::Failure("other reason", s.ToString());
    }
}

// Reference for RocksDB tuning: https://github.com/facebook/rocksdb/wiki/RocksDB-Tuning-Guide
// TODO: instrument for grid search over:
//       - memtable budget
//       - file size multiplier
//       - level/universal compaction
//       - compression per level
//       - block size

void ApplyColumnFamilyOptions(OpenMode mode, rocksdb::ColumnFamilyOptions& opts) {
    // level compaction, 1GiB memtable budget
    opts.OptimizeLevelStyleCompaction(1<<30);
    opts.num_levels = 5;
    // speeds ingestion but slows reads:
    // opts.target_file_size_multiplier = 4;
    // compress all files in 64KiB blocks with LZ4
    opts.compression_per_level.clear();
    opts.compression = rocksdb::kLZ4Compression;

    rocksdb::BlockBasedTableOptions bbto;
    bbto.format_version = 2;
    bbto.block_size = 64 * 1024;
    bbto.block_cache = rocksdb::NewLRUCache(totalRAM() / 4);

    opts.table_factory.reset(rocksdb::NewBlockBasedTableFactory(bbto));

    if (mode == OpenMode::BULK_LOAD) {
        // See also rocksdb::Options::PrepareForBulkLoad() in rocksdb/util/options.cc

        // Use RocksDB's vector memtable implementation instead of the default
        // skiplist. The vector has faster insertion but much slower lookup.
        opts.memtable_factory = std::make_shared<rocksdb::VectorRepFactory>();

        // Increase memtable size
        opts.write_buffer_size = totalRAM() / 8;
        opts.max_write_buffer_number = 6;
        opts.min_write_buffer_number_to_merge = 1;

        // never slowdown ingest.
        opts.level0_file_num_compaction_trigger = (1<<30);
        opts.level0_slowdown_writes_trigger = (1<<30);
        opts.level0_stop_writes_trigger = (1<<30);

        // A manual compaction run should pick all files in L0 in
        // a single compaction run.
        opts.source_compaction_factor = (1<<30);

        // We'll complete the bulk load in the DB destructor by performing a
        // full database compaction.
    }
}

void ApplyDBOptions(OpenMode mode, rocksdb::Options& opts) {
    ApplyColumnFamilyOptions(mode, static_cast<rocksdb::ColumnFamilyOptions&>(opts));

    opts.max_open_files = -1;

    // increase parallelism
    opts.max_background_compactions = std::min(std::thread::hardware_concurrency(), 16U);
    opts.max_background_flushes = std::min(std::thread::hardware_concurrency(), 4U);
    opts.env->SetBackgroundThreads(opts.max_background_compactions, rocksdb::Env::LOW);
    opts.env->SetBackgroundThreads(opts.max_background_flushes, rocksdb::Env::HIGH);

    if (mode == OpenMode::BULK_LOAD) {
        // See also rocksdb::Options::PrepareForBulkLoad() in rocksdb/util/options.cc
        opts.disable_auto_compactions = true;
        opts.disableDataSync = true;
    }
}

class Iterator : public KeyValue::Iterator {
private:
    std::unique_ptr<rocksdb::Iterator> iter_;
    std::string key_, value_;

    // No copying allowed
    Iterator(const Iterator&) = delete;
    void operator=(const Iterator&) = delete;

public:

    Iterator(std::unique_ptr<rocksdb::Iterator>&& iter) : iter_(move(iter)) {
        if (iter_->Valid()) {
            key_ = iter_->key().ToString();
            value_ = iter_->value().ToString();
        }
    }

    bool valid() override {
        return iter_->Valid();
    }

    const std::string& key() { return key_; }
    const std::string& value() { return value_; }

    Status next() override {
        if (!iter_->status().ok()) {
            return convertStatus(iter_->status());
        }
        iter_->Next();
        if (!iter_->status().ok()) {
            return convertStatus(iter_->status());
        }
        if (iter_->Valid()) {
            key_ = iter_->key().ToString();
            value_ = iter_->value().ToString();
        }
        return Status::OK();
    }
};


class Reader : public KeyValue::Reader {
private:
    rocksdb::DB* db_ = nullptr;

    // No copying allowed
    Reader(const Reader&) = delete;
    void operator=(const Reader&) = delete;

public:
    Reader() {}

    Reader(rocksdb::DB *db) {
        db_ = db;
    }

    ~Reader() {}

    Status get(KeyValue::CollectionHandle _coll,
               const std::string& key,
               std::string& value) const override {
        auto coll = reinterpret_cast<rocksdb::ColumnFamilyHandle*>(_coll);
        const rocksdb::ReadOptions r_options; // what should this be set to?
        std::string v_tmp;
        rocksdb::Status s = db_->Get(r_options, coll, key, &v_tmp);
        value = std::move(v_tmp);
        return convertStatus(s);
    }

    Status iterator(KeyValue::CollectionHandle _coll,
                    const std::string& key,
                    std::unique_ptr<KeyValue::Iterator>& it) const override {
        auto coll = reinterpret_cast<rocksdb::ColumnFamilyHandle*>(_coll);
        rocksdb::ReadOptions options;  // default values
        std::unique_ptr<rocksdb::Iterator> rit(db_->NewIterator(options, coll));
        if (!rit) {
            return Status::Failure("rocksdb::DB::NewIterator()");
        }
        if (key.empty()) {
            rit->SeekToFirst();
        } else {
            rit->Seek(key);
        }
        if (!rit->status().ok()) {
            return convertStatus(rit->status());
        }
        it = std::make_unique<Iterator>(move(rit));
        return Status::OK();
    }
};

class WriteBatch : public KeyValue::WriteBatch {
private:
    rocksdb::WriteBatch *wb_;
    rocksdb::DB* db_;
    const rocksdb::WriteOptions& batch_write_options_;
    friend class DB;

    // No copying allowed
    WriteBatch(const WriteBatch&);
    void operator=(const WriteBatch&);

public:
    WriteBatch(rocksdb::DB* db, const rocksdb::WriteOptions& batch_write_options)
        : db_(db), batch_write_options_(batch_write_options) {
        wb_ = new rocksdb::WriteBatch();
    }

    ~WriteBatch() {
        delete wb_;
        wb_ = nullptr; // extra sanitation
        db_ = nullptr;
    }

    Status put(KeyValue::CollectionHandle _coll,
               const std::string& key,
               const std::string& value) override {
        auto coll = reinterpret_cast<rocksdb::ColumnFamilyHandle*>(_coll);
        wb_->Put(coll, key, value);
        return Status::OK();
    }

    Status commit() override {
        rocksdb::Status s = db_->Write(batch_write_options_, wb_);
        return convertStatus(s);
    }
};

class DB : public KeyValue::DB {
private:
    rocksdb::DB* db_;
    std::map<const std::string, rocksdb::ColumnFamilyHandle*> coll2handle_;
    OpenMode mode_;
    rocksdb::WriteOptions write_options_, batch_write_options_;

    // No copying allowed
    DB(const DB&);
    void operator=(const DB&);

    DB(rocksdb::DB *db, std::map<const std::string, rocksdb::ColumnFamilyHandle*>& coll2handle,
       OpenMode mode)
        : db_(db), coll2handle_(std::move(coll2handle)),
          mode_(mode) {
            // prepare write options
            if (mode_ == OpenMode::BULK_LOAD) {
                write_options_.disableWAL = true;
                batch_write_options_.disableWAL = true;
            } else {
                batch_write_options_.sync = true;
            }
        }

public:
    static Status Initialize(const std::string& dbPath,
                       std::unique_ptr<KeyValue::DB> &db) {
        rocksdb::Options options;
        ApplyDBOptions(OpenMode::NORMAL, options);
        options.create_if_missing = true;
        options.error_if_exists = true;

        rocksdb::DB *rawdb = nullptr;
        rocksdb::Status s = rocksdb::DB::Open(options, dbPath, &rawdb);
        if (!s.ok()) {
            return convertStatus(s);
        }
        assert(rawdb != nullptr);

        std::map<const std::string, rocksdb::ColumnFamilyHandle*> coll2handle;
        db.reset(new DB(rawdb, coll2handle, OpenMode::NORMAL));
        if (!db) {
            delete rawdb;
            return Status::Failure();
        }
        return Status::OK();
    }

    static Status Open(const std::string& dbPath,
                       std::unique_ptr<KeyValue::DB> &db,
                       OpenMode mode) {
        // prepare options
        rocksdb::Options options;
        ApplyDBOptions(mode, options);
        options.create_if_missing = false;

        // detect the database's column families
        std::vector<std::string> column_family_names;
        rocksdb::Status s = rocksdb::DB::ListColumnFamilies(options, dbPath, &column_family_names);
        if (!s.ok()) {
            return convertStatus(s);
        }
        std::vector<rocksdb::ColumnFamilyDescriptor> column_families;
        for (const auto& nm : column_family_names) {
            rocksdb::ColumnFamilyOptions colopts;
            ApplyColumnFamilyOptions(mode, colopts);
            rocksdb::ColumnFamilyDescriptor cfd;
            cfd.name = nm;
            cfd.options = colopts;
            column_families.push_back(std::move(cfd));
        }

        // open the database (all column families)
        rocksdb::DB *rawdb = nullptr;
        std::vector<rocksdb::ColumnFamilyHandle*> column_family_handles;

        if (mode == OpenMode::READ_ONLY) {
            s = rocksdb::DB::OpenForReadOnly(options, dbPath, column_families,
                                             &column_family_handles, &rawdb);
        } else {
            s = rocksdb::DB::Open(options, dbPath, column_families,
                                  &column_family_handles, &rawdb);
        }
        if (!s.ok()) {
            return convertStatus(s);
        }
        assert(rawdb != nullptr);

        // create the database object with coll2handle_ pre-filled
        std::map<const std::string, rocksdb::ColumnFamilyHandle*> coll2handle;
        for (size_t i = 0; i < column_families.size(); i++) {
            coll2handle[column_family_names[i]] = column_family_handles[i];
        }
        db.reset(new DB(rawdb, coll2handle, mode));
        if (!db) {
            for (auto h : column_family_handles) {
                delete h;
            }
            delete rawdb;
            return Status::Failure();
        }

        return Status::OK();
    }

    ~DB() override {
        if (mode_ == OpenMode::BULK_LOAD) {
            // Complete the bulk load by compacting all the collections. This
            // can take a long time.
            for (const auto& p : coll2handle_) {
                db_->CompactRange(p.second, nullptr, nullptr);
            }
        }
        if (mode_ != OpenMode::READ_ONLY) {
            // Flush
            db_->SyncWAL();
            for (const auto& p : coll2handle_) {
                db_->Flush(rocksdb::FlushOptions(), p.second);
            }
        }
        // Free column handles
        for (const auto& p : coll2handle_) {
            delete p.second;
        }
        // delete database
        delete db_;
    }

    Status collection(const std::string& name,
                      KeyValue::CollectionHandle& coll) const override {
        auto p = coll2handle_.find(name);
        if (p != coll2handle_.end()) {
            coll = reinterpret_cast<KeyValue::CollectionHandle>(p->second);
            return Status::OK();
        }
        return Status::NotFound("column family does not exist", name);
    }

    Status create_collection(const std::string& name) override {
        if (coll2handle_.find(name) != coll2handle_.end()) {
            return Status::Exists("column family already exists", name);
        }

        // create new column family in rocksdb
        rocksdb::ColumnFamilyOptions colopts;
        ApplyColumnFamilyOptions(mode_, colopts);
        rocksdb::ColumnFamilyHandle *handle;
        rocksdb::Status s = db_->CreateColumnFamily(colopts, name, &handle);
        if (!s.ok()) {
            return convertStatus(s);
        }
        assert(handle != nullptr);

        // success, add a mapping from the column family name to the handle.
        coll2handle_[name] = handle;
        return Status::OK();
    }

    Status current(std::unique_ptr<KeyValue::Reader>& reader) const override {
        reader = std::make_unique<RocksKeyValue::Reader>(db_);
        return Status::OK();
    }

    Status begin_writes(std::unique_ptr<KeyValue::WriteBatch>& writes) override {
        writes = std::make_unique<RocksKeyValue::WriteBatch>(db_, batch_write_options_);
        return Status::OK();
    }

    Status get(KeyValue::CollectionHandle _coll,
               const std::string& key,
               std::string& value) const override {
        auto coll = reinterpret_cast<rocksdb::ColumnFamilyHandle*>(_coll);
        static const rocksdb::ReadOptions r_options; // what should this be set to?
        std::string v_tmp;
        rocksdb::Status s = db_->Get(r_options, coll, key, &v_tmp);
        value = std::move(v_tmp);
        return convertStatus(s);
    }

    Status put(KeyValue::CollectionHandle _coll,
               const std::string& key,
               const std::string& value) override {
        auto coll = reinterpret_cast<rocksdb::ColumnFamilyHandle*>(_coll);
        rocksdb::Status s = db_->Put(write_options_, coll, key, value);
        return convertStatus(s);
    }
};

Status Initialize(const std::string& dbPath, std::unique_ptr<KeyValue::DB>& db)
{
    return DB::Initialize(dbPath, db);
}

Status Open(const std::string& dbPath, std::unique_ptr<KeyValue::DB>& db, OpenMode mode)
{
    return DB::Open(dbPath, db, mode);
}

Status destroy(const std::string dbPath)
{
    rocksdb::Options options;
    Status s = convertStatus(rocksdb::DestroyDB(dbPath, options));
    ignore_retval(system(("rm -rf " + dbPath).c_str()));
    return s;
}

}}
