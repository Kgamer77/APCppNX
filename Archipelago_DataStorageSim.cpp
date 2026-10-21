#include <cstdint>
#include <cstdio>
#include <functional>
// #include <json/value.h>
// #include <json/writer.h>
#include <cJSON/cJSON.h>
#include <map>
#include <string>

// Macros for bulding cJSON objects
#define ADD_STR_LITERAL(root, name, value) \
cJSON_AddItemToObject(root, name, cJSON_CreateString(#value));
#define ADD_STRING(root, name, value) \
cJSON_AddItemToObject(root, name, cJSON_CreateString(value));
#define ADD_NUMBER(root, name, value) \
cJSON_AddItemToObject(root, name, cJSON_CreateNumber(value));
#define ADD_ARRAY(root, name, type, values, count) \
cJSON_AddItemToObject(root, #name, cJSON_Create##type##Array(##values, ##count));
#define ADD_OBJECT(root, name, value) \
cJSON_AddItemToObject(root, #name, ##value);
#define ADD_BOOL(root, name, value) \
cJSON_AddBoolToObject(root, name, value);
#define ADD_RAW(root, name, value) \
cJSON_AddRawToObject(root, #name, ##value);
#define APPEND_STRING(array, value) \
cJSON_AddItemToArray(array, cJSON_CreateString(value));
#define APPEND_NUMBER(array, value) \
cJSON_AddItemToArray(array, cJSON_CreateNumber(value));

extern cJSON *sp_save_root;
extern std::string sp_save_path;

extern std::function<void(std::string)> logfunc;

extern const int AP_OFFLINE_SLOT;

void WriteFileJSON(cJSON *root, std::string path);
bool parse_response(std::string msg, std::string &request);

void operation_replace(std::string key, cJSON *new_val) {
    cJSON_AddItemToObject(sp_save_root, "store", new_val);
    cJSON *store = cJSON_GetObjectItem(sp_save_root, "store");
    cJSON *value = cJSON_GetObjectItem(store, key.c_str());
    cJSON_Delete(value);
    cJSON_AddItemToObject(store, key.c_str(), new_val);
}
void operation_or(std::string key, cJSON *new_val) {
    cJSON *store = cJSON_GetObjectItem(sp_save_root, "store");
    cJSON *value = cJSON_GetObjectItem(store, key.c_str());

    // Semi-broken implementation. Assumes integer or bool values atm
    if (cJSON_IsNumber(value)) {
        // int64_t cmpValue = value->valueint;
        cJSON_SetNumberValue(value, (value->valueint | new_val->valueint));
    } else if (cJSON_IsBool(value)) {
        bool cmpValue = cJSON_IsTrue(value);
        cJSON_SetBoolValue(value, (cJSON_IsTrue(value) || cJSON_IsTrue(new_val)));
    }

}
void operation_default(std::string key, cJSON *new_val) {
    // Noop as default is set in resolveDataStorageOp.
}

static std::map<const std::string, const std::function<void(std::string,cJSON*)>> known_operations = {
    {"replace", operation_replace},
    {"or", operation_or},
    {"default", operation_default},
};

void resolveDataStorageOp(cJSON *dataOp) {
    cJSON *store = cJSON_GetObjectItem(sp_save_root, "store");
    std::string command = cJSON_GetObjectItem(dataOp, "cmd")->valuestring;
    if (command == "Set") {
        std::string key = cJSON_GetObjectItem(dataOp, "key")->valuestring;
        cJSON *original_value = cJSON_GetObjectItem(store, key.c_str());
        cJSON *defaultValue = cJSON_GetObjectItem(dataOp, "default");
        if (!original_value && defaultValue) {
            // Key not in store. Set default
            // finish handling other types
            switch (defaultValue->type) {
                case cJSON_False:
                ADD_BOOL(store, key.c_str(), cJSON_IsTrue(defaultValue))
                break;
                case cJSON_True:
                ADD_BOOL(store, key.c_str(), cJSON_IsTrue(defaultValue))
                break;

                case cJSON_Number:
                ADD_NUMBER(store, key.c_str(), defaultValue->valueint);
                break;

                case cJSON_String:
                ADD_STRING(store, key.c_str(), defaultValue->valuestring)
                break;

                case cJSON_Array:
                case cJSON_Object:
                case cJSON_Raw:
            }
        }
        cJSON *operations = cJSON_GetObjectItem(dataOp, "operations");
        cJSON *op = NULL;
        if (operations)
        cJSON_ArrayForEach(op, operations) {
            std::string operation_str = cJSON_GetObjectItem(op, "operation")->valuestring;
            cJSON *newVal = cJSON_GetObjectItem(op, "value");
            if (known_operations.count(operation_str))
                known_operations[operation_str](key, newVal);
            else
                logfunc("AP: Unimplement Datastore operation " + operation_str +  " in offline session!");
        }
        WriteFileJSON(sp_save_root, sp_save_path);
        bool wantReply = cJSON_IsTrue(cJSON_GetObjectItem(op, "want_reply"));
        if (wantReply) {
            cJSON *fake_msg = cJSON_CreateArray();
            cJSON *req_t = cJSON_CreateObject();
            std::string req;
            ADD_STR_LITERAL(req_t, "cmd", SetReply)
            ADD_STRING(req_t, "key", key.c_str())
            cJSON *value = cJSON_GetObjectItem(store, "value");
            switch (value->type) {
                case cJSON_False:
                ADD_BOOL(req_t, key.c_str(), cJSON_IsTrue(value))
                break;
                case cJSON_True:
                ADD_BOOL(req_t, key.c_str(), cJSON_IsTrue(value))
                break;

                case cJSON_Number:
                ADD_NUMBER(req_t, key.c_str(), value->valueint);
                break;

                case cJSON_String:
                ADD_STRING(req_t, key.c_str(), value->valuestring)
                break;

                case cJSON_Array:
                case cJSON_Object:
                case cJSON_Raw:
            }
            switch (original_value->type) {
                case cJSON_False:
                ADD_BOOL(req_t, key.c_str(), cJSON_IsTrue(value))
                break;
                case cJSON_True:
                ADD_BOOL(req_t, key.c_str(), cJSON_IsTrue(value))
                break;

                case cJSON_Number:
                ADD_NUMBER(req_t, key.c_str(), value->valueint);
                break;

                case cJSON_String:
                ADD_STRING(req_t, key.c_str(), value->valuestring)
                break;

                case cJSON_Array:
                case cJSON_Object:
                case cJSON_Raw:
            }
            ADD_NUMBER(req_t, "slot", AP_OFFLINE_SLOT)
            cJSON_AddItemToArray(fake_msg, req_t);
            parse_response(cJSON_Print(fake_msg), req);
            cJSON_Delete(fake_msg);
        }
    } else if (command == "Get") {
        cJSON *fake_msg = cJSON_CreateArray();
        cJSON *req_t = cJSON_CreateObject();
        std::string req;
        ADD_STR_LITERAL(req_t, "cmd", Retrieved)
        cJSON *keys = cJSON_AddObjectToObject(req_t, "keys");
        cJSON *key = NULL;
        cJSON_ArrayForEach(key, cJSON_GetObjectItem(dataOp, "keys")) {
            std::string keyVal = key->valuestring;
            cJSON *saveValue = cJSON_GetObjectItem(sp_save_root, keyVal.c_str());
            switch (saveValue->type) {
                case cJSON_False:
                ADD_BOOL(keys, keyVal.c_str(), cJSON_IsTrue(saveValue))
                break;
                case cJSON_True:
                ADD_BOOL(keys, keyVal.c_str(), cJSON_IsTrue(saveValue))
                break;

                case cJSON_Number:
                ADD_NUMBER(keys, keyVal.c_str(), saveValue->valueint);
                break;

                case cJSON_String:
                ADD_STRING(keys, keyVal.c_str(), saveValue->valuestring)
                break;

                case cJSON_Array:
                case cJSON_Object:
                case cJSON_Raw:
            }

        }
        cJSON_AddItemToArray(fake_msg, req_t);
        parse_response(cJSON_Print(fake_msg), req);
        cJSON_Delete(fake_msg);
    }
    cJSON_Delete(dataOp);
}
