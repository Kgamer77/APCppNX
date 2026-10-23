#include "Archipelago.h"

// #include "ixwebsocket/IXNetSystem.h"
// #include "ixwebsocket/IXWebSocket.h"
// #include "ixwebsocket/IXUserAgent.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
// #include <iostream>
#include <queue>
#include <random>
// #include <fstream>
// #include <json/json.h>
// #include <json/reader.h>
// #include <json/value.h>
// #include <json/writer.h>
#include "cJSON/cJSON.h"
#include "cJSON/cJSON_Utils.h"
#include <deque>
#include <set>
#include <string>
#include <chrono>
#include <functional>
#include <utility>
#include <vector>
#include <algorithm>
#include <filesystem>
#include "Randomizer/Randomizer.h"
#include "SDRedirect/SDPaths.h"
#include "fsHelper.h" 
#include "Archipelago/WebSocketClient.hpp"
#include "Archipelago/ArchipelagoClient.hpp"

extern const int AP_OFFLINE_SLOT = 1404;
constexpr int AP_OFFLINE_TEAM = 0;
constexpr char const* AP_OFFLINE_NAME = "You";
constexpr AP_NetworkVersion AP_DEFAULT_NETWORK_VERSION = {0,5,1}; // Default for compatibility reasons

enum ProtocolPackets {
    RoomInfo,
    ConnectionRefused,
    Connected,
    ReceivedItems,
    LocationInfo,
    RoomUpdate,
    PrintJSON,
    DataPackage,
    Bounced,
    InvalidPacket,
    Retrieved,
    SetReply,
    Connect,
    ConnectUpdate,
    Sync,
    LocationChecks,
    LocaionScouts,
    CreateHints,
    UpdateHint,
    StatusUpdate,
    Say,
    GetPackageData,
    Bounce,
    Get,
    Set,
    SetNotify
};

//Setup Stuff
std::function<void(std::string)> logfunc = [](std::string log){ Randomizer::logPush("%s", log.c_str()); };
bool init = false;
bool auth = false;
bool refused = false;
bool multiworld = true;
bool isSSL = true;
bool ssl_success = false;
int ap_player_id;
int ap_player_team;
std::string ap_player_name;
size_t ap_player_name_hash;
std::string ap_ip;
std::string ap_game;
std::string ap_passwd;
std::uint64_t ap_uuid = 0;
std::mt19937_64 rando;
AP_NetworkVersion client_version = AP_DEFAULT_NETWORK_VERSION; 

//Deathlink Stuff
bool deathlinkstat = false;
bool deathlinksupported = false;
bool enable_deathlink = false;
int deathlink_amnesty = 0;
int cur_deathlink_amnesty = 0;

// Message System
std::deque<AP_Message*> messageQueue;
bool queueitemrecvmsg = true;

// Data Maps
std::map<int, AP_NetworkPlayer> map_players;
std::map<std::pair<std::string,int64_t>, std::string> map_location_id_name;
std::map<std::pair<std::string,int64_t>, std::string> map_item_id_name;

// Data Sets
set<int> teams_set;

// Callback function pointers
std::function<void()> resetItemValues = nullptr;
std::function<void(int64_t,bool)> getitemfunc = nullptr;
std::function<void(int64_t)> checklocfunc = nullptr;
std::function<void(vector<AP_NetworkItem>)> locinfofunc = nullptr;
std::function<void(std::string, std::string)> recvdeath = nullptr;
std::function<void(AP_SetReply)> setreplyfunc = nullptr;
std::function<void(AP_Bounce)> bouncedfunc = nullptr;

// Serverdata Management
std::map<std::string,AP_DataType> map_serverdata_typemanage;
AP_GetServerDataRequest resync_serverdata_request;
uint32_t last_item_idx = 0;

void resolveDataStorageOp(cJSON *op);

// Gifting interop
bool gifting_supported = false;
bool gifting_autoReject = true;
void handleGiftAPISetReply(const AP_SetReply& reply);

// Singleplayer Seed Info
std::string sp_save_path;
cJSON *sp_save_root;

//Misc Data for Clients
AP_RoomInfo lib_room_info;

//Server Data Stuff
std::map<std::string, AP_GetServerDataRequest*> map_server_data;
std::queue<std::pair<cJSON*,AP_RequestStatus*>> queue_server_data;

//Slot Data Stuff
std::map<std::string, std::function<void(int)>> map_slotdata_callback_int;
std::map<std::string, std::function<void(cJSON*)>> map_slotdata_callback_raw;
std::map<std::string, std::function<void(std::map<int,int>)>> map_slotdata_callback_mapintint;

// Datapackage Stuff
std::filesystem::path datapkg_cache_dir = "sd:/SMO-Archipelago/Multi-Data/.datapkg-cache/";
set<std::string> datapkg_outdated_games;

WebSocketClient *websocket;

cJSON *sp_ap_root;

// PRIV Func Declarations Start
void AP_Init_Generic();
bool parse_response(std::string msg, std::string &request);
void APSend(std::string req);
void WriteFileJSON(cJSON *json, std::string path);
std::string getItemName(std::string game, int64_t id);
std::string getLocationName(std::string game, int64_t id);
AP_NetworkPlayer getPlayer(int team, int slot);
bool loadDataPkg(const std::string& game, const std::string& hash);
void cacheDataPkgs(cJSON* serverPkgs);
cJSON *getDataPkgRequest(void);
// PRIV Func Declarations End

void AP_Init(const char* ip, u16 port, const char* game, const char* player_name, const char* passwd) {
    multiworld = true;
    
    uint64_t milliseconds_since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    rando = std::mt19937_64(milliseconds_since_epoch);

    if (!strcmp(ip,"")) {
        ip = "archipelago.gg:38281";
        logfunc("AP: Using default Server Adress: '" + std::string(ip) + "'");
    } else {
        logfunc("AP: Using Server Adress: '" + std::string(ip) + "'");
    }
    ap_ip = ip;
    ap_game = game;
    ap_player_name = player_name;
    ap_passwd = passwd;

    logfunc("AP: Initializing...");

    //Connect to server
    // Wrap websocketclient in higher level similar to SMOO Client
    // ix::initNetSystem();
    ArchipelagoClient* client = ArchipelagoClient::instance();
    client->setHostName("wss://" + ap_ip);
    client->setPort(port);
    // webSocket.setUrl();
    client->setMessageCallback([](const char* msg, u64 msgLength)
        {
            // if (msg->type == ix::WebSocketMessageType::Message)
            // {
            std::string request;
            if (parse_response(msg, request)) {
                APSend(request);
            }
            // }
            // else if (msg->type == ix::WebSocketMessageType::Open)
            // {
            //     logfunc("AP: Connected to Archipelago");
            // }
            // else if (msg->type == ix::WebSocketMessageType::Error || msg->type == ix::WebSocketMessageType::Close)
            // {
            //     auth = false;
            //     for (std::pair<std::string,AP_GetServerDataRequest*> itr : map_server_data) {
            //         itr.second->status = AP_RequestStatus::Error;
            //         map_server_data.erase(itr.first);
            //     }
            //     logfunc("AP: Error connecting to Archipelago. Retries: " + std::to_string(msg->errorInfo.retries-1));
            //     if (msg->errorInfo.retries-1 >= 2 && isSSL && !ssl_success) {
            //         logfunc("AP: SSL connection failed. Attempting unencrypted...");
            //         webSocket.setUrl("ws://" + ap_ip);
            //         isSSL = false;
            //     }
            // }
        }
    );
    
    client->setPerMessageDeflate(true);
    client->setPingInterval(45);

    AP_NetworkPlayer archipelago {
        -1,
        0,
        "Archipelago",
        "Archipelago",
        "__Server"

    };
    map_players[0] = archipelago;
    AP_Init_Generic();
}

void AP_Init(const char* filename) {
    multiworld = false;
    nn::fs::FileHandle handle;
    
    
    nn::Result result = nn::fs::OpenFile(&handle, filename, nn::fs::OpenMode_Read);
    if (result.IsFailure()) {
        return;
        // result = nn::fs::CreateFile(filename.c_str(), data.size());
        // if (result.IsFailure())
        //     return;
    }
    
    long fileSize = 0;
    nn::fs::GetFileSize(&fileSize, handle);
    std::string mwFile;
    mwFile.reserve(fileSize + 1);
    // handle, 0, data.c_str(), data.size(), nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush)
    
    result = nn::fs::ReadFile(handle, 0, mwFile.data(), fileSize);
    if (result.IsFailure())
        return;
    
    nn::fs::CloseFile(handle);

    sp_ap_root = cJSON_ParseWithLength(mwFile.c_str(), mwFile.size());
    sp_save_path = std::string(filename) + ".save";
    result = nn::fs::OpenFile(&handle, sp_save_path.c_str(), nn::fs::OpenMode_Read);
    if (result.IsFailure()) {
        // Save file does not exist
        return;
    }
    nn::fs::GetFileSize(&fileSize, handle);
    std::string saveFile;
    saveFile.reserve(fileSize + 1);

    result = nn::fs::ReadFile(handle, 0, saveFile.data(), fileSize);
    if (result.IsFailure())
        return;
    
    nn::fs::CloseFile(handle);

    sp_save_root = cJSON_ParseWithLength(saveFile.c_str(), saveFile.size());

    WriteFileJSON(sp_save_root, sp_save_path);
    ap_player_name = AP_OFFLINE_NAME;
    AP_Init_Generic();
}

void AP_Start() {
    init = true;
    if (multiworld) {
        ArchipelagoClient::instance()->initConnection();
    } else {
        cJSON *data_package = cJSON_AddObjectToObject(sp_ap_root, "data_package");
        // needs to be finished
        // False or Null
        cJSON *initItem = cJSON_GetObjectItem(sp_save_root, "init");
        if (!cJSON_IsTrue(initItem)) {
            // Null
            if (initItem) {
                ADD_BOOL(sp_save_root, "init", true)
                cJSON_AddArrayToObject(sp_save_root, "checked_locations");
                cJSON_AddObjectToObject(sp_save_root, "store");
            } else { // False
                cJSON_SetBoolValue(initItem, true);
                if (!cJSON_GetObjectItemCaseSensitive(sp_save_root, "checked_locations"))
                    cJSON_AddArrayToObject(sp_save_root, "checked_locations");
                if (!cJSON_GetObjectItemCaseSensitive(sp_save_root, "store"))
                    cJSON_AddObjectToObject(sp_save_root, "store");
            }
            
        }
        // Only game in the data package is our game
        
        ap_game = cJSON_GetArrayItem(
         cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(sp_ap_root, "data_package"), "data"),
         "games"), 0)->string;
        cJSON *fake_msg = cJSON_CreateArray();
        cJSON *command = cJSON_CreateObject();
        ADD_STRING(command, "cmd", "Connected")
        ADD_NUMBER(command, "team", AP_OFFLINE_TEAM)
        ADD_NUMBER(command, "slot", AP_OFFLINE_SLOT)
        cJSON *slotInfo = cJSON_AddObjectToObject(command, "slot_info");
        cJSON *info = cJSON_CreateObject();
        ADD_STRING(info, "game", ap_game.c_str())
        cJSON_AddItemToObject(slotInfo, std::to_string(AP_OFFLINE_SLOT).c_str(), info);
        cJSON *players = cJSON_AddArrayToObject(command, "players");
        cJSON *netPlayer = cJSON_CreateObject();
        ADD_NUMBER(netPlayer, "team", AP_OFFLINE_TEAM)
        ADD_NUMBER(netPlayer, "slot", AP_OFFLINE_SLOT)
        ADD_STRING(netPlayer, "alias", AP_OFFLINE_NAME)
        ADD_STRING(netPlayer, "name", AP_OFFLINE_NAME)
        cJSON *checkedLocs = cJSON_AddArrayToObject(command, "checked_locations");
        cJSON *savedLocs = cJSON_GetObjectItem(sp_save_root, "checked_locations");
        cJSON *element = NULL;
        cJSON_ArrayForEach(element, savedLocs) {
            APPEND_NUMBER(checkedLocs, element->valueint)
        }

        cJSON *slotData = cJSON_AddObjectToObject(command, "slot_data");
        cJSON *apSlotData = cJSON_GetObjectItem(sp_ap_root, "slot_data");
        element = NULL;
        cJSON_ArrayForEach(element, apSlotData) {
            switch (element->type) {
                case cJSON_String:
                ADD_STRING(slotData, element->string, element->valuestring)
                break;
                case cJSON_Number:
                ADD_NUMBER(slotData, element->string, element->valueint)
                break;
            }
        }
        cJSON_AddItemToArray(fake_msg, command);
        std::string req;
        parse_response(cJSON_Print(fake_msg), req);
        cJSON_Delete(fake_msg);

        fake_msg = cJSON_CreateArray();
        command = cJSON_CreateObject();
        ADD_STRING(command, "cmd", "DataPackage")
        cJSON *ourPackage = cJSON_GetObjectItem(cJSON_GetObjectItem(sp_ap_root, "data_package"), "data");
        data_package = cJSON_Parse(cJSON_Print(ourPackage->child));
        ADD_OBJECT(command, "data", data_package);
        cJSON_AddItemToArray(fake_msg, command);
        parse_response(cJSON_Print(fake_msg), req);
        cJSON_Delete(fake_msg);

        fake_msg = cJSON_CreateArray();
        command = cJSON_CreateObject();
        ADD_STRING(command, "cmd", "ReceivedItems")
        ADD_NUMBER(command, "index", 0)
        cJSON *items = cJSON_AddArrayToObject(command, "items");
        savedLocs = cJSON_GetObjectItem(sp_save_root, "checked_locations");
        cJSON *locationToItem = cJSON_GetObjectItem(sp_ap_root, "location_to_item");
        element = NULL;
        cJSON_ArrayForEach(element, savedLocs) {
            cJSON *netItem = cJSON_CreateObject();
            ADD_NUMBER(netItem, "item", cJSON_GetObjectItem(locationToItem, std::to_string(element->valueint).c_str())->valueint)
            ADD_NUMBER(netItem, "location", 0)
            ADD_NUMBER(netItem, "player", ap_player_id)
            cJSON_AddItemToArray(items, netItem);
        }
        cJSON_AddItemToArray(fake_msg, command);
        parse_response(cJSON_Print(fake_msg), req);
        cJSON_Delete(fake_msg);
    }
}

void AP_Shutdown() {
    if (multiworld)
        ArchipelagoClient::instance()->disconnect();

    // Reset all states
    init = false;
    auth = false;
    refused = false;
    multiworld = true;
    isSSL = true;
    ssl_success = false;
    ap_player_id = 0;
    ap_player_name.clear();
    ap_ip.clear();
    ap_game.clear();
    ap_passwd.clear();
    ap_uuid = 0;
    client_version = AP_DEFAULT_NETWORK_VERSION;
    deathlinkstat = false;
    deathlinksupported = false;
    enable_deathlink = false;
    deathlink_amnesty = 0;
    cur_deathlink_amnesty = 0;
    gifting_autoReject = true;
    gifting_supported = false;
    while (AP_IsMessagePending()) AP_ClearLatestMessage();
    queueitemrecvmsg = true;
    map_players.clear();
    map_location_id_name.clear();
    map_item_id_name.clear();
    resetItemValues = nullptr;
    getitemfunc = nullptr;
    checklocfunc = nullptr;
    locinfofunc = nullptr;
    recvdeath = nullptr;
    setreplyfunc = nullptr;
    map_serverdata_typemanage.clear();
    last_item_idx = 0;
    sp_save_path.clear();
    cJSON_Delete(sp_save_root);
    map_server_data.clear(); // Does this leak?
    map_slotdata_callback_int.clear();
    map_slotdata_callback_raw.clear();
    map_slotdata_callback_mapintint.clear();
    // clean up sp_ap_root
    cJSON_Delete(sp_ap_root);
}

bool AP_IsInit() {
    return init;
}

void AP_SetClientVersion(AP_NetworkVersion* version) {
    client_version.major = version->major;
    client_version.minor = version->minor;
    client_version.build = version->build;
}

void AP_SendItem(int64_t idx) {
    AP_SendItem(set<int64_t>{ idx });
}
void AP_SendItem(set<int64_t> const& locations) {
    for (int64_t idx : locations) {
        logfunc("AP: Checked '" + getLocationName(ap_game, idx) + "'.");
    }
    if (multiworld) {
        cJSON *req_t = cJSON_CreateArray();
        cJSON *locationChecks = cJSON_CreateObject();

        ADD_STRING(locationChecks, "cmd", "LocationChecks")
        vector<int> locs;
        for (int64_t loc : locations) {
            locs.push_back(loc);
        };
        ADD_ARRAY(locationChecks, "locations", Int, locs.data(), locs.size())
        cJSON_AddItemToArray(req_t, locationChecks);
        APSend(cJSON_Print(req_t));
        cJSON_Delete(req_t);
    } else {
        set<int64_t> new_locations;
        cJSON *checkedLocations = cJSON_GetObjectItem(sp_save_root, "checked_locations");
        cJSON *element = NULL;
        for (int64_t idx : locations) {
            bool was_previously_checked = false;
            cJSON_ArrayForEach(element, checkedLocations) {
                if (element->valueint == idx) {
                    was_previously_checked = true;
                    break;
                }
            }
            if (!was_previously_checked) new_locations.insert(idx);
        }

        cJSON *fake_msg = cJSON_CreateArray();
        cJSON *recItems = cJSON_CreateObject();
        ADD_STRING(recItems, "cmd", "ReceivedItems")
        ADD_NUMBER(recItems, "index", last_item_idx+1)
        cJSON *items = cJSON_AddArrayToObject(recItems, "items");
        cJSON *locationToItem = cJSON_GetObjectItem(sp_ap_root, "location_to_item");
        for (int64_t idx : new_locations) {
            int64_t recv_item_id = cJSON_GetObjectItem(locationToItem, std::to_string(idx).c_str())->valueint;
            if (recv_item_id == 0) continue;
            cJSON *item = cJSON_CreateObject();
            ADD_NUMBER(item, "item", recv_item_id)
            ADD_NUMBER(item, "location", idx)
            ADD_NUMBER(item, "player", ap_player_id)
            cJSON_AddItemToArray(items, item);
        }
        std::string req;
        cJSON_AddItemToArray(fake_msg, recItems);
        parse_response(cJSON_Print(fake_msg), req);
        cJSON_Delete(fake_msg);

        fake_msg = cJSON_CreateArray();
        recItems = cJSON_CreateObject();
        ADD_STRING(recItems, "cmd", "RoomUpdate")
        items = cJSON_AddArrayToObject(recItems, "checked_locations");
        checkedLocations = cJSON_GetObjectItem(sp_save_root, "checked_locations");

        for (int64_t idx : new_locations) {
            APPEND_NUMBER(items, idx)
            APPEND_NUMBER(checkedLocations, idx);
        }
        cJSON_AddItemToArray(fake_msg, recItems);
        WriteFileJSON(sp_save_root, sp_save_path);
        parse_response(cJSON_Print(fake_msg), req);
    }
}

void AP_SendLocationScouts(set<int64_t> const& locations, int create_as_hint) {
    if (multiworld) {
        cJSON *req_t = cJSON_CreateArray();
        cJSON *locationScouts = cJSON_CreateObject();
        ADD_STRING(locationScouts, "cmd", "LocationScouts")
        cJSON *locs = cJSON_AddArrayToObject(locationScouts, "locations");
        for (int64_t loc : locations) {
            APPEND_NUMBER(locs, loc)
        }
        ADD_NUMBER(locationScouts, "create_as_hint", create_as_hint)
        cJSON_AddItemToArray(req_t, locationScouts);
        APSend(cJSON_Print(req_t));
        cJSON_Delete(req_t);
    } else {
        cJSON *fake_msg = cJSON_CreateArray();
        cJSON *locationInfo = cJSON_CreateObject();
        ADD_STRING(locationInfo, "cmd", "LocationInfo")
        cJSON *locs = cJSON_AddArrayToObject(locationInfo, "locations");
        cJSON *locationToItem = cJSON_GetObjectItem(sp_ap_root, "location_to_item");
        for (int64_t loc : locations) {
            cJSON *netitem = cJSON_CreateObject();
            ADD_NUMBER(netitem, "item", cJSON_GetObjectItem(locationToItem, std::to_string(loc).c_str())->valueint)
            ADD_NUMBER(netitem, "location", loc)
            ADD_NUMBER(netitem, "player", ap_player_id)
            ADD_NUMBER(netitem, "flags", 0b001) // Hardcoded for SP seeds.
            cJSON_AddItemToArray(locs, netitem);
        }
    }
}

void AP_UpdateTags(vector<std::string> const& tags)
{
    lib_room_info.tags = tags;
    if (!multiworld) return;
    vector<const char*> cstrTags;
    cJSON *req_t = cJSON_CreateObject();
    ADD_STRING(req_t, "cmd", "ConnectUpdate")
    for (std::string tag : tags) {
        cstrTags.push_back(tag.c_str());
    }
    ADD_ARRAY(req_t, "tags", String, cstrTags.data(), cstrTags.size())
    APSend(cJSON_Print(req_t));
    cJSON_Delete(req_t);
}

void AP_StoryComplete() {
    if (!multiworld) return;
    cJSON *req_t = cJSON_CreateArray();
    cJSON *statusUpdate = cJSON_CreateObject();
    ADD_STRING(statusUpdate, "cmd", "StatusUpdate")
    ADD_NUMBER(statusUpdate, "status", 30) // CLIENT_GOAL
    cJSON_AddItemToArray(req_t, statusUpdate);
    APSend(cJSON_Print(req_t));
    cJSON_Delete(req_t);
}

void AP_DeathLinkSend(const std::string &cause) {
    if (!enable_deathlink || !multiworld) return;
    if (cur_deathlink_amnesty > 0) {
        cur_deathlink_amnesty--;
        return;
    }
    cur_deathlink_amnesty = deathlink_amnesty;
    std::chrono::time_point<std::chrono::system_clock> timestamp = std::chrono::system_clock::now();
    AP_Bounce b;
    cJSON *v = cJSON_CreateObject();
    ADD_NUMBER(v, "time", (int64_t)std::chrono::duration_cast<std::chrono::seconds>(timestamp.time_since_epoch()).count())
    ADD_STRING(v, "source", ap_player_name.c_str()) // Name and Shame >:D
    if (!cause.empty())
    {
        std::string cause_pname = cause;
        constexpr std::string_view pname_you{"%YOU%"};
        size_t pname_marker = cause_pname.find(pname_you);

        if (pname_marker != std::string::npos)
            cause_pname.replace(pname_marker, pname_you.size(), ap_player_name);
        ADD_STRING(v, "cause", cause_pname.c_str())
    }
    b.data = cJSON_Print(v);
    b.games = nullptr;
    b.slots = nullptr;
    vector<const char*> tags = {"DeathLink"};
    b.tags = &tags;
    AP_SendBounce(b);
    cJSON_Delete(v);
}

void AP_EnableQueueItemRecvMsgs(bool b) {
    queueitemrecvmsg = b;
}

void AP_SetLoggingCallback(std::function<void(std::string)> f_log) {
    logfunc = f_log;
}

void AP_SetItemClearCallback(std::function<void()> f_itemclr) {
    resetItemValues = f_itemclr;
}

void AP_SetItemRecvCallback(std::function<void(int64_t,bool)> f_itemrecv) {
    getitemfunc = f_itemrecv;
}

void AP_SetLocationCheckedCallback(std::function<void(int64_t)> f_locrecv) {
    checklocfunc = f_locrecv;
}

void AP_SetLocationInfoCallback(std::function<void(vector<AP_NetworkItem>)> f_locinfrecv) {
    locinfofunc = f_locinfrecv;
}

void AP_SetDeathLinkRecvCallback(std::function<void()> f_deathrecv) {
    recvdeath = [f_deathrecv](std::string, std::string){ f_deathrecv(); };
}
void AP_SetDeathLinkRecvCallback(std::function<void(std::string, std::string)> f_deathrecv) {
    recvdeath = f_deathrecv;
}

void AP_RegisterSlotDataIntCallback(std::string key, std::function<void(int)> f_slotdata) {
    map_slotdata_callback_int[key] = f_slotdata;
}

void AP_RegisterSlotDataRawCallback(std::string key, std::function<void(cJSON*)> f_slotdata) {
    map_slotdata_callback_raw[key] = f_slotdata;
}

void AP_RegisterSlotDataMapIntIntCallback(std::string key, std::function<void(std::map<int,int>)> f_slotdata) {
    map_slotdata_callback_mapintint[key] = f_slotdata;
}

void AP_SetDeathLinkSupported(bool supdeathlink) {
    deathlinksupported = supdeathlink;
}

bool AP_DeathLinkPending() {
    return deathlinkstat;
}

void AP_DeathLinkClear() {
    deathlinkstat = false;
}

bool AP_IsMessagePending() {
    return !messageQueue.empty();
}

AP_Message* AP_GetLatestMessage() {
    return messageQueue.front();
}

void AP_ClearLatestMessage() {
    if (AP_IsMessagePending()) {
        delete messageQueue.front();
        messageQueue.pop_front();
    }
}

void AP_Say(std::string text) {
    cJSON *req_t = cJSON_CreateArray();
    cJSON *say = cJSON_CreateObject();
    ADD_STRING(say, "cmd", "Say")
    ADD_STRING(say, "text", text.c_str())

    cJSON_AddItemToArray(req_t, say);

    std::string request = cJSON_Print(req_t);

    APSend(request);
    cJSON_Delete(req_t);
}

int AP_GetRoomInfo(AP_RoomInfo* client_roominfo) {
    if (!auth) return 1;
    *client_roominfo = lib_room_info;
    return 0;
}

AP_ConnectionStatus AP_GetConnectionStatus() {
    if (!multiworld && auth) return AP_ConnectionStatus::Authenticated;
    if (refused) {
        return AP_ConnectionStatus::ConnectionRefused;
    }
    if (ArchipelagoClient::instance()->isSocketOpen()) {
        if (auth) {
            return AP_ConnectionStatus::Authenticated;
        } else {
            return AP_ConnectionStatus::Connected;
        }
    }
    return AP_ConnectionStatus::Disconnected;
}

std::uint64_t AP_GetUUID() {
    return ap_uuid;
}

int AP_GetPlayerID() {
    return ap_player_id;
}

void AP_BulkSetServerData(AP_SetServerDataRequest* request) {
    request->status = AP_RequestStatus::Pending;

    cJSON *req_t = cJSON_CreateObject();
    ADD_STRING(req_t, "cmd", "Set")
    ADD_STRING(req_t, "key", request->key.c_str())
    cJSON *ops = cJSON_AddArrayToObject(req_t, "operations");
    cJSON *item = NULL;
    switch (request->type) {
        case AP_DataType::Int:
            for (int i = 0; i < request->operations.size(); i++) {
                item = cJSON_CreateObject();
                ADD_STRING(item, "operation", request->operations[i].operation.c_str())
                ADD_NUMBER(item, "value", *((int*)request->operations[i].value))
                cJSON_AddItemToArray(ops, item);
            }
            if (request->default_value != nullptr) {
                ADD_NUMBER(item, "default", *((int*)request->default_value))
            }
            break;
        case AP_DataType::Double:
            for (int i = 0; i < request->operations.size(); i++) {
                item = cJSON_CreateObject();
                ADD_STRING(item, "operation", request->operations[i].operation.c_str())
                ADD_NUMBER(item, "value", *((double*)request->operations[i].value))
                cJSON_AddItemToArray(ops, item);
            }
            if (request->default_value != nullptr) {
                ADD_NUMBER(item, "default", *((double*)request->default_value))
            }
            break;
        default:
            for (int i = 0; i < request->operations.size(); i++) {
                item = cJSON_CreateObject();
                ADD_STRING(item, "operation", request->operations[i].operation.c_str())
                cJSON *data = cJSON_ParseWithLength((*(std::string*)request->operations[i].value).c_str(),((std::string*)request->operations[i].value)->size());
                cJSON_AddItemToObject(item, "value", data);
            }
            if (request->default_value != nullptr) {
                cJSON *default_val_json = cJSON_ParseWithLength((*(std::string*)request->default_value).c_str(),((std::string*)request->default_value)->size());
                cJSON_AddItemToObject(item, "default", default_val_json);
            }
            break;
    }
    ADD_BOOL(req_t, "want_reply", request->want_reply)
    map_serverdata_typemanage[request->key] = request->type;

    queue_server_data.push({req_t,&request->status});
}

void AP_CommitServerData() {
    cJSON *req = cJSON_CreateArray();

    while (!queue_server_data.empty()) {
        std::pair<cJSON*, AP_RequestStatus*> request = queue_server_data.front();
        cJSON_AddItemToArray(req, request.first);
        std::string key = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(req, cJSON_GetArraySize(req) - 1), "cmd")->valuestring;

        if (key == "Set" || key == "SetNotify") // Set has local completion at this stage
            *(request.second) = AP_RequestStatus::Done;
        if (!multiworld)
            resolveDataStorageOp(request.first);
        queue_server_data.pop();
    }
    if (multiworld) APSend(cJSON_Print(req));
    // This should also delete all popped request items as they
    // were made subentities of req in the while loop
    cJSON_Delete(req);
}

void AP_SetServerData(AP_SetServerDataRequest* request) {
    AP_BulkSetServerData(request);
    AP_CommitServerData();
}

void AP_RegisterSetReplyCallback(std::function<void(AP_SetReply)> f_setreply) {
    setreplyfunc = f_setreply;
}

void AP_SetNotify(std::map<std::string,AP_DataType> keylist, bool requestCurrentValue) {
    cJSON *req_t = cJSON_CreateObject();
    ADD_STRING(req_t, "cmd", "SetNotify")
    cJSON *keys = cJSON_AddArrayToObject(req_t, "keys");

    std::string zero = "0";
    std::string emptyJson = "{}";

    int i = 0;
    vector<AP_SetServerDataRequest> requests;

    for (std::pair<std::string,AP_DataType> keytypepair : keylist) {
        APPEND_STRING(keys, keytypepair.first.c_str());
        map_serverdata_typemanage[keytypepair.first] = keytypepair.second;

        i++;

        if (requestCurrentValue) {
            AP_SetServerDataRequest setDefaultRequest;
            setDefaultRequest.key = keytypepair.first;
            setDefaultRequest.type = keytypepair.second;
            setDefaultRequest.want_reply = true;
            switch (keytypepair.second) {
                case AP_DataType::Int:
                case AP_DataType::Double:
                    setDefaultRequest.operations = {{"default", &zero}};
                    setDefaultRequest.default_value = &zero;
                    break;
                case AP_DataType::Raw:
                    setDefaultRequest.operations = {{"default", &zero}};
                    setDefaultRequest.default_value = &emptyJson;
                    break;
            }
            
            requests.push_back(setDefaultRequest);
        }
    }

    AP_RequestStatus requestStatus;
    queue_server_data.push({req_t, &requestStatus});

    for (AP_SetServerDataRequest& request : requests)
        AP_BulkSetServerData(&request);

    AP_CommitServerData();
}

void AP_SetNotify(std::string key, AP_DataType type, bool requestCurrentValue) {
    std::map<std::string,AP_DataType> keylist;
    keylist[key] = type;
    AP_SetNotify(keylist, requestCurrentValue);
}

void AP_BulkGetServerData(AP_GetServerDataRequest* request) {
    request->status = AP_RequestStatus::Pending;

    if (map_server_data.find(request->key) != map_server_data.end()) return;

    map_server_data[request->key] = request;

    cJSON *req_t = cJSON_CreateObject();
    ADD_STRING(req_t, "cmd", "Get")
    ADD_STRING(req_t, "keys", request->key.c_str())


    queue_server_data.push({req_t,&request->status});
}

void AP_GetServerData(AP_GetServerDataRequest *request) {
    AP_BulkGetServerData(request);
    AP_CommitServerData();
}

std::string AP_GetPrivateServerDataPrefix() {
    return "APCpp" + std::to_string(ap_player_name_hash) + "APCpp" + std::to_string(ap_player_id) + "APCpp";
}

void AP_SendBounce(AP_Bounce bounce) {
    cJSON *req_t = cJSON_CreateObject();
    ADD_STRING(req_t, "cmd", "Bounce")

    // Add targets for bounce, if requested
    #define ADD_TARGETS( targets, type ) \
            if (bounce.targets != nullptr && !bounce.targets->empty()) { \
            ADD_ARRAY(req_t, #targets, type, bounce.targets->data(), bounce.targets->size() ) \
            }

    ADD_ARRAY(req_t, "tags", Int, bounce.slots->data(), bounce.slots->size() ) 


    ADD_TARGETS(games, String)
    ADD_TARGETS(slots, Int)
    ADD_TARGETS(tags, String)
    #undef ADD_TARGETS
    ADD_RAW(req_t, "data", bounce.data.c_str())

    APSend(cJSON_Print(req_t));
    cJSON_Delete(req_t);
}

void AP_RegisterBouncedCallback(std::function<void(AP_Bounce)> f_bounced) {
    bouncedfunc = f_bounced;
}

// PRIV

void AP_Init_Generic() {
    ap_player_name_hash = std::hash<std::string>{}(ap_player_name);
    SDPaths::ensureArchipelagoRoot();
    nn::fs::CreateDirectory(datapkg_cache_dir.c_str());
}

bool parse_response(std::string msg, std::string &request) {
    // sample Archipelago protocol packet,
    // [{"cmd": "RoomInfo",
    // "version": {"major": 0, "minor": 1, "build": 3, "class": "Version"}, "tags": ["WebHost"], ... }]
    cJSON *root = cJSON_ParseWithLength(msg.c_str(), msg.size());
    cJSON *network_cmd = NULL;
    cJSON *command_data = NULL;
    // Iterate through protocol packet object
    // starting with iterating through each network command
    std::string cmd;
    cJSON_ArrayForEach(network_cmd, root) {
        // Iterate through each data member of a network command
        cJSON_ArrayForEach(command_data, network_cmd) {
            if (strcmp("cmd", command_data->string) == 0) {
                cmd = cJSON_GetStringValue(command_data);
            }
            if (cmd == "RoomInfo") {
                if (strcmp("version", command_data->string) == 0) {
                    cJSON *version_data = command_data->child;
                    lib_room_info.version.major = cJSON_GetObjectItemCaseSensitive(version_data, "major")->valueint;
                    lib_room_info.version.minor = cJSON_GetObjectItemCaseSensitive(version_data, "minor")->valueint;
                    lib_room_info.version.build = cJSON_GetObjectItemCaseSensitive(version_data, "build")->valueint;
                }
                else if (strcmp("tags", command_data->string) == 0 ) {
                    cJSON *tags_data = cJSON_GetObjectItemCaseSensitive(command_data, "tags");
                    cJSON *tag = NULL;
                    vector<std::string> serv_tags;
                    cJSON_ArrayForEach(tag, tags_data) {
                        serv_tags.push_back(tag->valuestring);
                    }
                    lib_room_info.tags = serv_tags;
                }
                else if (strcmp("password", command_data->string) == 0) {
                    // might need to be casted to bool
                    lib_room_info.password_required = cJSON_IsTrue(command_data);
                }
                else if (strcmp("permissions", command_data->string) == 0) {
                    std::map<std::string,int> serv_permissions;
                    cJSON *permission = NULL;
                    cJSON_ArrayForEach(permission, command_data) {
                        serv_permissions[permission->valuestring] = permission->valueint;
                    }
                    lib_room_info.permissions = serv_permissions;
                }
                else if (strcmp("hint_cost", command_data->string) == 0) {
                    
                    lib_room_info.hint_cost = command_data->valueint;
                }
                else if (strcmp("location_check_points", command_data->string) == 0) {
                    lib_room_info.location_check_points = command_data->valueint;
                }
                else if (strcmp("games", command_data->string) == 0) {

                }
                else if (strcmp("datapackage_checksums", command_data->string) == 0) {
                    std::map<std::string,std::string> serv_datapkg_checksums;
                    cJSON *checksum = NULL;
                    cJSON_ArrayForEach(checksum, command_data) {
                        serv_datapkg_checksums[checksum->string] = checksum->valuestring;
                    }
                    lib_room_info.datapackage_checksums = serv_datapkg_checksums;
                }
                else if (strcmp("seed_name", command_data->string) == 0) {
                    lib_room_info.seed_name = command_data->valuestring;
                }
                else if (strcmp("time", command_data->string) == 0) {
                    lib_room_info.time = command_data->valuedouble;
                }
            } else if (cmd == "Connected") {
                // might need to guarantee players is processed before slot_info
                // Avoid inconsistency if we disconnected before
                logfunc("AP: Authenticated");
                if (strcmp("team", command_data->string) == 0) {
                    ap_player_team = command_data->valueint;
                }
                else if (strcmp("slot", command_data->string) == 0) {
                    // MUST be called before resetitemvalues, otherwise PrivateServerDataPrefix, 
                    // GetPlayerID return broken values!
                    ap_player_id = command_data->valueint;
                    resetItemValues();
                }
                else if (strcmp("players", command_data->string) == 0) {
                    cJSON *player = NULL;
                    cJSON_ArrayForEach(player, command_data) {
                        AP_NetworkPlayer networkPlayer = {   
                            cJSON_GetObjectItemCaseSensitive(player, "team")->valueint,
                            cJSON_GetObjectItemCaseSensitive(player, "slot")->valueint,
                            cJSON_GetObjectItemCaseSensitive(player, "name")->valuestring,
                            cJSON_GetObjectItemCaseSensitive(player, "alias")->valuestring,
                            "PLACEHOLDER"
                        };
                        map_players[networkPlayer.slot] = networkPlayer;
                        teams_set.insert(networkPlayer.team);
                    }
                }
                else if (strcmp("missing_locations", command_data->string) == 0) {}
                else if (strcmp("checked_locations", command_data->string) == 0) {
                    cJSON *missingLocation = NULL;
                    cJSON_ArrayForEach(missingLocation, command_data) {
                        //Sync checks with server
                        int64_t loc_id = missingLocation->valueint;
                        checklocfunc(loc_id);
                    }
                }
                else if (strcmp("slot_data", command_data->string) == 0) {
                    cJSON *slotData = NULL;
                    cJSON_ArrayForEach(slotData, command_data) {
                        // DeathLink data members
                        if (deathlinksupported) {
                            if (strcmp("death_link", slotData->string) == 0 || strcmp("DeathLink", slotData->string) == 0) {
                                enable_deathlink = cJSON_IsTrue(slotData);
                            }
                            if (strcmp("death_link_amnesty", slotData->string) == 0) {
                                deathlink_amnesty = slotData->valueint;
                                cur_deathlink_amnesty = deathlink_amnesty;
                            }
                        }

                        // Custom slot data
                        std::string key = slotData->string;
                        if (map_slotdata_callback_int.count(key)) {
                            map_slotdata_callback_int[key](slotData->valueint);
                        } else if (map_slotdata_callback_raw.count(key)) {
                            // pass the child ptr of the current object instead of deserializing and reserializing the json
                            map_slotdata_callback_raw[key](slotData->child);
                        } else if (map_slotdata_callback_mapintint.count(key)) {
                            std::map<int,int> out;
                            cJSON *mapObj = NULL;
                            cJSON_ArrayForEach(mapObj, slotData) {
                                out[std::stoi(mapObj->string)] = mapObj->valueint;
                            }
                            map_slotdata_callback_mapintint[key](out);
                        } else {
                            if (key != "death_link" && key != "death_link_amnesty" && key != "DeathLink" && key != "DeathLink_Amnesty")
                                logfunc("AP: Warning: Unmapped slot data with key '" + key + "'!");
                        }
                        
                    }
                }
                else if (strcmp("slot_info", command_data->string) == 0) {
                    cJSON *networkSlot = NULL;
                    // Slots are 1 indexed not 0 indexed since slot 0 is always the archipelago server's slot
                    int slot = 1;
                    cJSON_ArrayForEach(networkSlot, command_data) {
                        // might need to find a way to sanity check a slot exists in the map
                        if (map_players.find(slot) != map_players.end()) {
                            map_players[slot].game = cJSON_GetObjectItemCaseSensitive(networkSlot, "game")->valuestring;
                        }
                        slot += 1;
                    }
                }
                else if (strcmp("hint_points", command_data->string) == 0) {
                    // Add hint points data member
                }


                
                

                
                
                // Look into gifting before attemtting to bridge support 
                // if (gifting_supported) {
                //     // Order is important, Motherboxes must be retrieved before personal box for auto-rejection reasons, do not combine
                //     std::map<std::string,AP_DataType> giftMotherBoxKeys;
                //     for (int team : teams_set)
                //         giftMotherBoxKeys.emplace("GiftBoxes;" + std::to_string(team), AP_DataType::Raw); 
                //     AP_SetNotify(giftMotherBoxKeys, true);
                //     AP_SetNotify("GiftBox;" + std::to_string(ap_player_team) + ";" + std::to_string(ap_player_id), AP_DataType::Raw, true);
                // }

                

                resync_serverdata_request.key = "APCppLastRecv" + ap_player_name + std::to_string(ap_player_id);
                resync_serverdata_request.value = &last_item_idx;
                resync_serverdata_request.type = AP_DataType::Int;
                AP_GetServerData(&resync_serverdata_request);

                cJSON *req_t = cJSON_CreateArray();
                if (enable_deathlink && deathlinksupported) {
                    cJSON *connUpdate = cJSON_CreateObject();
                    vector<const char*> tags;
                    tags.push_back("DeathLink");
                    ADD_STRING(connUpdate, "cmd", "ConnectUpdate")
                    ADD_ARRAY(connUpdate, "tags", String, tags.data(), tags.size())
                    cJSON_AddItemToArray(req_t, connUpdate);
                }

                for (auto &game_pkg : lib_room_info.datapackage_checksums) {
                    if (!loadDataPkg(game_pkg.first, game_pkg.second)) {
                        datapkg_outdated_games.insert(game_pkg.first);
                    }
                }

                // getDataPkgRequest returns either a Sync or GetDataPackage packet
                cJSON_AddItemToArray(req_t, getDataPkgRequest());
                request = cJSON_Print(req_t);
                cJSON_Delete(req_t);
                cJSON_Delete(root);

                return true;
            } else if (cmd == "DataPackage") {
                if (strcmp("data", command_data->string) == 0) {
                    cacheDataPkgs(command_data->child);
                    cJSON *req_t = cJSON_CreateArray();
                    cJSON_AddItemToArray(req_t, getDataPkgRequest());
                    // Possibly Change to PrintUnformatted to save space
                    request = cJSON_Print(req_t);
                    cJSON_Delete(root);
                    cJSON_Delete(req_t);
                    return true;
                }
            } else if (cmd == "Retrieved") {
                if (strcmp(command_data->string, "keys") == 0) {
                    cJSON *keys = NULL;

                    cJSON_ArrayForEach(keys, command_data) {
                        if (!map_server_data.count(keys->string)) continue;
                        AP_GetServerDataRequest* target = map_server_data[keys->string];
                        switch (target->type) {
                        case AP_DataType::Int:
                            *((int*)target->value) = keys->valueint;
                            break;
                        case AP_DataType::Double:
                            *((double*)target->value) = keys->valuedouble;
                            break;
                        case AP_DataType::Raw:
                            *((std::string*)target->value) = cJSON_Print(keys);
                            break;
                        }
                        target->status = AP_RequestStatus::Done;
                        map_server_data.erase(keys->string);
                    }
                    break;
                }
            } else if (cmd == "SetReply") {
                if (strcmp(command_data->string, "key") == 0) {
                   if (gifting_supported && strcmp(command_data->valuestring, "GiftBox") == 0) {
                        // Reserved by library. Used for Gifting API
                        std::string raw_val;
                        std::string raw_orig_val;
                        AP_SetReply setreply;
                        raw_val = cJSON_Print(cJSON_GetObjectItem(network_cmd, "value"));
                        raw_orig_val = cJSON_Print(cJSON_GetObjectItem(network_cmd, "original_value"));
                        setreply.key = command_data->valuestring;
                        setreply.value = &raw_val;
                        setreply.original_value = &raw_orig_val;
                        handleGiftAPISetReply(setreply);
                    }

                    if (setreplyfunc) {
                        int int_val;
                        int int_orig_val;
                        double dbl_val;
                        double dbl_orig_val;
                        std::string raw_val;
                        std::string raw_orig_val;
                        AP_SetReply setreply;
                        setreply.key = command_data->valuestring;
                        switch (map_serverdata_typemanage[setreply.key]) {
                            case AP_DataType::Int:
                                int_val = cJSON_GetObjectItem(network_cmd, "value")->valueint;
                                int_orig_val = cJSON_GetObjectItem(network_cmd, "original_value")->valueint;
                                setreply.value = &int_val;
                                setreply.original_value = &int_orig_val;
                                break;
                            case AP_DataType::Double:
                                dbl_val = cJSON_GetObjectItem(network_cmd, "value")->valuedouble;
                                dbl_orig_val = cJSON_GetObjectItem(network_cmd, "original_value")->valuedouble;
                                setreply.value = &dbl_val;
                                setreply.original_value = &dbl_orig_val;
                                break;
                            default:
                                raw_val = cJSON_Print(cJSON_GetObjectItem(network_cmd, "value"));
                                raw_orig_val = cJSON_Print(cJSON_GetObjectItem(network_cmd, "original_value"));
                                setreply.value = &raw_val;
                                setreply.original_value = &raw_orig_val;
                                break;
                        }
                        setreplyfunc(setreply);
                    }
                }
            } else if (cmd == "PrintJSON") {
                const std::string printType = cJSON_GetObjectItem(network_cmd, "type")->valuestring;
                if (printType == "ItemSend" || printType == "ItemCheat") {
                    int recvPlr = cJSON_GetObjectItem(network_cmd, "receiving")->valueint;
                    // Filter out itemrecv messages, which would otherwise be duplicated from the itemrecv callback
                    if (getPlayer(0, recvPlr).alias == getPlayer(0, ap_player_id).alias) continue;
                    AP_NetworkPlayer recv_player = getPlayer(0, recvPlr);
                    AP_ItemSendMessage* msg = new AP_ItemSendMessage;
                    msg->type = AP_MessageType::ItemSend;
                    msg->item = getItemName(recv_player.game, cJSON_GetObjectItem(cJSON_GetObjectItem(network_cmd, "item"), "item")->valueint);
                    msg->recvPlayer = recv_player.alias;
                    msg->text = msg->item + std::string(" was sent to ") + msg->recvPlayer;
                    messageQueue.push_back(msg);
                } else if (printType == "Hint") {
                    int recvPlr = cJSON_GetObjectItem(network_cmd, "receiving")->valueint;
                    cJSON *netItem = cJSON_GetObjectItem(network_cmd, "item");
                    
                    AP_NetworkPlayer send_player = getPlayer(0, cJSON_GetObjectItem(netItem, "player")->valueint);
                    AP_NetworkPlayer recv_player = getPlayer(0, recvPlr);
                    AP_HintMessage* msg = new AP_HintMessage;
                    msg->type = AP_MessageType::Hint;
                    msg->item = getItemName(recv_player.game, cJSON_GetObjectItem(netItem, "item")->valueint);
                    msg->sendPlayer = send_player.alias;
                    msg->recvPlayer = recv_player.alias;
                    msg->location = getLocationName(send_player.game, cJSON_GetObjectItem(netItem, "location")->valueint);
                    msg->checked = cJSON_IsTrue(cJSON_GetObjectItem(network_cmd, "found"));
                    msg->text = std::string("Item ") + msg->item + std::string(" from ") + msg->sendPlayer + std::string(" to ") + msg->recvPlayer + std::string(" at ") + msg->location + std::string((msg->checked ? " (Checked)" : " (Unchecked)"));
                    messageQueue.push_back(msg);
                } else if (printType == "Countdown") {
                    AP_CountdownMessage* msg = new AP_CountdownMessage;
                    msg->type = AP_MessageType::Countdown;
                    if (cJSON_GetObjectItem(network_cmd, "countdown")->valueint) {
                        msg->timer = cJSON_GetObjectItem(network_cmd, "countdown")->valueint;
                    } else {
                        msg->timer = 0;
                    }
                    msg->text = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(network_cmd, "data"), 0), "text")->valuestring;
                    messageQueue.push_back(msg);
                } else if (printType == "Chat") {
                    AP_NetworkPlayer sender = getPlayer(0, cJSON_GetObjectItem(network_cmd, "slot")->valueint);
                    AP_ChatMessage* msg = new AP_ChatMessage;
                    msg->type = AP_MessageType::Chat;
                    msg->player = sender.alias;
                    msg->message = cJSON_GetObjectItem(network_cmd, "message")->valuestring;
                    msg->text = msg->player + ": " + msg->message;
                    messageQueue.push_back(msg);
                } else if (printType == "ServerChat") {
                    AP_ServerChatMessage* msg = new AP_ServerChatMessage;
                    msg->type = AP_MessageType::ServerChat;
                    msg->message = cJSON_GetObjectItem(network_cmd, "message")->valuestring;
                    msg->text = "[Server]: " + msg->message;
                    messageQueue.push_back(msg);
                } else {
                    if (strcmp(command_data->string, "data") == 0) {
                        AP_Message* msg = new AP_Message;
                        msg->text = "";
                        cJSON *messagePart = NULL;
                        // iterate through JSONMessageParts in data
                        cJSON_ArrayForEach(messagePart, command_data) {
                            std::string type = cJSON_GetObjectItem(messagePart, "type")->valuestring;
                            cJSON *textJSON = cJSON_GetObjectItem(messagePart, "text");
                            std::string text = "";
                            if (cJSON_IsString(textJSON))
                                text = textJSON->valuestring;
                            if (type == "player_id") {
                                msg->text += getPlayer(0, textJSON->valueint).alias;
                            } else if (text != "") {
                                msg->text += text;
                            }
                        }
                        messageQueue.push_back(msg);
                    }
                }
            } else if (cmd == "LocationInfo") {
                vector<AP_NetworkItem> locations;
                // only one data member locations
                cJSON *netItem = NULL;
                cJSON_ArrayForEach(netItem, command_data) {
                    AP_NetworkItem item;
                    item.item = cJSON_GetObjectItem(netItem, "item")->valueint;
                    item.location = cJSON_GetObjectItem(netItem, "location")->valueint;
                    AP_NetworkPlayer player = getPlayer(0, cJSON_GetObjectItem(netItem, "player")->valueint);
                    item.player = player.slot;
                    item.flags = cJSON_GetObjectItem(netItem, "flags")->valueint;
                    item.itemName = getItemName(player.game, item.item);
                    item.locationName = getLocationName(ap_game, item.location);
                    item.playerName = player.alias;
                    locations.push_back(item);
                }
                if (locinfofunc) {
                    locinfofunc(locations);
                } else {
                    logfunc("AP: Received LocationInfo but no handler registered!");
                }
                break;
            } else if (cmd == "ReceivedItems") {
                cJSON *netItem = NULL;
                int item_idx = cJSON_GetObjectItem(network_cmd, "index")->valueint;
                bool notify;
                int cur_index = 0;
                cJSON_ArrayForEach(netItem, cJSON_GetObjectItem(network_cmd, "items")) {
                    int64_t item_id = cJSON_GetObjectItem(netItem, "item")->valueint;
                    notify = (item_idx == 0 && last_item_idx <= cur_index && multiworld) || item_idx != 0;
                    getitemfunc(item_id, notify);
                    if (queueitemrecvmsg && notify) {
                        AP_ItemRecvMessage* msg = new AP_ItemRecvMessage;
                        AP_NetworkPlayer sender = getPlayer(0, cJSON_GetObjectItem(netItem, "player")->valueint);
                        msg->type = AP_MessageType::ItemRecv;
                        msg->item = getItemName(ap_game, item_id);
                        msg->sendPlayer = sender.alias;
                        msg->text = std::string("Received ") + msg->item + std::string(" from ") + msg->sendPlayer;
                        messageQueue.push_back(msg);
                    }
                    cur_index++;
                }
                last_item_idx = item_idx == 0 ? cur_index : last_item_idx + cur_index;
                AP_SetServerDataRequest request;
                request.key = "APCppLastRecv" + ap_player_name + std::to_string(ap_player_id);
                AP_DataStorageOperation replac;
                replac.operation = "replace";
                replac.value = &last_item_idx;
                vector<AP_DataStorageOperation> operations;
                operations.push_back(replac);
                request.operations = operations;
                request.default_value = 0;
                request.type = AP_DataType::Int;
                request.want_reply = false;
                AP_SetServerData(&request);
                break;
            } else if (cmd == "RoomUpdate") {
                //Sync checks with server
                if (strcmp(command_data->string, "checked_locations") == 0) {
                    cJSON *location = NULL; 
                    cJSON_ArrayForEach(location, command_data) {
                        int64_t loc_id = location->valueint;
                        checklocfunc(loc_id);
                    }
                }

                //Update Player aliases if present
                //Sync checks with server
                if (strcmp(command_data->string, "players") == 0) {
                    cJSON *player = NULL; 
                    cJSON_ArrayForEach(player, command_data) {
                        map_players[cJSON_GetObjectItem(player, "slot")->valueint].alias = cJSON_GetObjectItem(player, "alias")->valuestring;
                    }
                }
            } else if (cmd == "ConnectionRefused") {
                auth = false;
                refused = true;
                logfunc("AP: Archipelago Server has refused connection. Check Password / Name / IP and restart the Game.");
                //fflush(stdout);
            } else if (cmd == "Bounced") {
                if (!enable_deathlink && !bouncedfunc) continue;
                if (!bouncedfunc) {
                    // Only do native DeathLink handling, client is not interested in bounce packets
                    cJSON *tag = NULL;
                    cJSON_ArrayForEach(tag, cJSON_GetObjectItem(network_cmd, "tags")) {
                        std::string tag_val = tag->valuestring;
                        if (tag_val == "DeathLink") {
                            std::string source = cJSON_GetObjectItem(cJSON_GetObjectItem(network_cmd, "data"), "source")->valuestring;
                            
                            // Suspicions confirmed ;-; But maybe we died, not them?
                            if (source == ap_player_name) break; // We already paid our penance
                            deathlinkstat = true;
                            if (recvdeath) {
                                std::string cause = cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(network_cmd, "data"), "cause")) ? "" : cJSON_GetObjectItem(cJSON_GetObjectItem(network_cmd, "data"), "cause")->valuestring;
                                recvdeath(source, cause);
                            }
                            break;
                        }
                    }
                    break;
                } else {
                    AP_Bounce bounce;
                    vector<const char*> games;
                    vector<int> slots;
                    vector<const char*> tags;
                    cJSON *arg = NULL; 
                    cJSON *target = NULL;
                    // Add targets to bounce package
                    #define ADD_TARGETS( targets, accessor ) \
                            arg = cJSON_GetObjectItem(network_cmd, #targets); \
                            target = NULL; \
                            if (arg) { \
                                cJSON_ArrayForEach(target, cJSON_GetObjectItem(network_cmd, #targets)) { \
                                        targets.push_back(target->accessor); \
                                } \
                                bounce.targets = &targets; \
                            } 
                    ADD_TARGETS(games, valuestring)
                    ADD_TARGETS(slots, valueint)
                    ADD_TARGETS(tags, valuestring)
                    #undef ADD_TARGETS

                    bounce.data = cJSON_Print(cJSON_GetObjectItem(network_cmd, "data"));
                    bouncedfunc(bounce);
                    break;
                }
                
            }
                
            // send the Connect packet after RoomInfo processed
            if (!auth && cmd == "RoomInfo") {
                cJSON *req_t = cJSON_CreateObject();
                
                ap_uuid = rando();
                ADD_STRING(req_t, "cmd", "Connect")
                ADD_STRING(req_t, "game", ap_game.c_str())
                ADD_STRING(req_t, "name", ap_player_name.c_str())
                ADD_STRING(req_t, "password", ap_passwd.c_str())
                ADD_NUMBER(req_t, "uuid", ap_uuid)
                vector<std::string> tags;
                vector<const char*> tags_cstr;
                ADD_ARRAY(req_t, "tags", String, tags_cstr.data(), tags.size())
                cJSON *version = cJSON_CreateObject();
                ADD_NUMBER(version, "major", client_version.major)
                ADD_NUMBER(version, "minor", client_version.minor)
                ADD_NUMBER(version, "build", client_version.build)
                ADD_STRING(version, "class", "Version")
                ADD_OBJECT(req_t, "version", version)
                // Full remote items
                ADD_NUMBER(req_t, "items_handling", 7)
                request = cJSON_Print(req_t);
                cJSON_Delete(req_t);
                cJSON_Delete(root);
                return true;
            }
        }

        
    }
    cJSON_Delete(root);
    return false;
}


void APSend(std::string req) {
    if (ArchipelagoClient::isSocketOpen()) {
        logfunc("AP: Not Connected. Send will fail.");
        return;
    }
    ArchipelagoClient *client = ArchipelagoClient::instance();
    client->sendMessage(req);
}

void WriteFileJSON(cJSON *json, std::string path) {
    nn::fs::FileHandle handle;
    
    std::string data =  cJSON_PrintUnformatted(json);

    nn::Result result = nn::fs::OpenFile(&handle, path.c_str(), nn::fs::OpenMode_Write);
    if (result.IsFailure()) {
        result = nn::fs::CreateFile(path.c_str(), data.size());
        if (result.IsFailure())
            return;
    }

    result = nn::fs::WriteFile(handle, 0, data.c_str(), data.size(), nn::fs::WriteOption::CreateOption(nn::fs::WriteOptionFlag_Flush));
    if (result.IsFailure())
        return;
    
    nn::fs::CloseFile(handle);
}

std::filesystem::path getDataPkgCachePath(const std::string& game, const std::string& hash)
{
    std::string alphanum_game, alphanum_hash;

    auto alphanum_func = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    std::copy_if(game.begin(), game.end(), std::back_inserter(alphanum_game), alphanum_func);
    std::copy_if(hash.begin(), hash.end(), std::back_inserter(alphanum_hash), alphanum_func);

    return datapkg_cache_dir / (alphanum_game + "-" + alphanum_hash + ".json");
}

void parseDataPkg(const std::string& game, cJSON *package)
{
    cJSON *location_name_to_id = cJSON_GetObjectItemCaseSensitive(package, "location_name_to_id");
    cJSON *item_name_to_id = cJSON_GetObjectItemCaseSensitive(package, "item_name_to_id");
    cJSON *entry = NULL;

    cJSON_ArrayForEach(entry, location_name_to_id) {
        map_location_id_name[{game, entry->valueint}] = entry->string;
    }

    entry = NULL;
    cJSON_ArrayForEach(entry, item_name_to_id) {
        map_item_id_name[{game, entry->valueint}] = entry->string;
    }
}

bool loadDataPkg(const std::string& game, const std::string& hash) {
    std::filesystem::path cache_path = getDataPkgCachePath(game, hash);

    if (!FsHelper::isFileExist(cache_path.c_str())) {
        return false;
    }

    FsHelper::LoadData loadData = {.path = cache_path.c_str() };

    FsHelper::loadFileFromPath(loadData);

    std::string cache_file = static_cast<const char*>(loadData.buffer);
    
    cJSON *datapkg = cJSON_ParseWithLength(cache_file.c_str(), cache_file.size());
    parseDataPkg(game, datapkg);

    return true;
}

void cacheDataPkgs(cJSON *serverPkgs) {
    cJSON *gamePkg = NULL;
    cJSON_ArrayForEach(gamePkg, serverPkgs) {
        std::string game = gamePkg->string;
        std::string hash = cJSON_GetObjectItemCaseSensitive(gamePkg, "checksum")->valuestring;
        std::filesystem::path cache_path = getDataPkgCachePath(game, hash);
        parseDataPkg(game, gamePkg);
        WriteFileJSON(gamePkg, cache_path.string());

        datapkg_outdated_games.erase(game);
        logfunc("AP: Game Cache updated for " + game);
    }
}

cJSON* getDataPkgRequest(void) {
    cJSON* server_req = cJSON_CreateObject();

    if (datapkg_outdated_games.empty()) {
        ADD_STRING(server_req, "cmd", "Sync")

        auth = true;
        ssl_success = auth && isSSL;
        refused = false;
    }
    else {
        // Fetch multiple games from the server at once to take advantage of compression.
        // Fetch up to 3 games at a time. Except if exactly 4 are left; then do 2 and 2 instead.
        int num_to_fetch = (datapkg_outdated_games.size() == 4 ? 2 : (std::min)(3, (int)datapkg_outdated_games.size()));

        ADD_STRING(server_req, "cmd", "GetDataPackage")
        
        
        vector<const char*> games;
        
        for (const std::string& game : datapkg_outdated_games) {
            games.push_back(game.c_str());
            if (!(--num_to_fetch > 0))
            break;
        }
        ADD_ARRAY(server_req, "games", String, games.data(), games.size())

    }
    return server_req;
}

std::string getItemName(std::string game, int64_t id) {
    std::pair<std::string,int64_t> item = {game,id};
    return map_item_id_name.count(item) ? map_item_id_name.at(item) : std::string("Unknown Item") + std::to_string(id) + " from " + game;
}

std::string getLocationName(std::string game, int64_t id) {
    std::pair<std::string,int64_t> location = {game,id};
    return map_location_id_name.count(location) ? map_location_id_name.at(location) : std::string("Unknown Location") + std::to_string(id) + " from " + game;
}

AP_NetworkPlayer getPlayer(int team, int slot) {
    return map_players[slot];
}

AP_NetworkPlayer getPlayer(int team, std::string name) {
    for (std::pair<int,AP_NetworkPlayer> player : map_players) {
        if (player.second.name == name && player.second.team == team) {
            return player.second;
        }
    }
    static AP_NetworkPlayer ERR_Player = {
        -1,
        -1,
        "ERR",
        "ERR",
        "ERR"
    };
    return ERR_Player;
}
