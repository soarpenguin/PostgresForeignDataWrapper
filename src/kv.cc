
#include "rocksdb/db.h"
#include "rocksdb/options.h"
using namespace rocksdb;
using namespace std;

#include "kv.h"

extern "C" {

void* Open(char* path) {
    DB* db = nullptr;
    Options options;
    options.IncreaseParallelism();
    options.create_if_missing = true;
    Status s = DB::Open(options, string(path), &db);
    assert(s.ok());
    return db;
}

void Close(void* db) {
    if (db) {
        delete static_cast<DB*>(db);
    }
}

uint64 Count(void* db) {
    string num;
    static_cast<DB*>(db)->GetProperty("rocksdb.estimate-num-keys", &num);
    return stoull(num);
}

void* GetIter(void* db) {
    Iterator* it = static_cast<DB*>(db)->NewIterator(ReadOptions());
    it->SeekToFirst();
    return it;
}

void DelIter(void* it) {
    if (it) {
        delete static_cast<Iterator*>(it);
    }
}

bool Next(void* db, void* iter, char** key, uint32* keyLen,
          char** value, uint32* valLen) {
    Iterator* it = static_cast<Iterator*>(iter);
    if (!it->Valid()) return false;
    *keyLen = it->key().size(), *valLen = it->value().size();
    *key = (char*) palloc0(*keyLen);
    *value = (char*) palloc0(*valLen);
    strncpy(*key, it->key().data(), *keyLen);
    strncpy(*value, it->value().data(), *valLen);
    it->Next();
    return true;
}

bool Get(void* db, char* key, uint32 keyLen, char** value, uint32* valLen) {
    string sval;
    Status s = static_cast<DB*>(db)->Get(ReadOptions(), Slice(key, keyLen), &sval);
    if (!s.ok()) return false;
    *valLen = sval.length();
    *value = (char*) palloc0(*valLen);
    strncpy(*value, sval.c_str(), *valLen);
    return true;
}

bool Put(void* db, char* key, uint32 keyLen, char* value, uint32 valLen) {
    Status s = static_cast<DB*>(db)->Put(WriteOptions(), Slice(key, keyLen),
                                         Slice(value, valLen));
    return s.ok()? true: false;
}

bool Delete(void* db, char* key, uint32 keyLen) {
    Status s = static_cast<DB*>(db)->Delete(WriteOptions(), Slice(key, keyLen));
    return s.ok()? true: false;
}

}
