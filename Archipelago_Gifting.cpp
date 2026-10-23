#include "Archipelago.h"
// #include <json/json.h>
// #include <json/value.h>
// #include <json/writer.h>
#include <cJSON.h>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <format>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <mutex>
#include "Archipelago/ArchipelagoSocket.h"

// extern Json::FastWriter writer;
// extern Json::Reader reader;
extern std::mt19937_64 rando;
extern int ap_player_team;
extern set<int> teams_set;
extern bool gifting_supported;
extern bool gifting_autoReject;

extern std::function<void(std::string)> logfunc;

// Stuff that is only used for Gifting
std::map<std::pair<int,std::string>,AP_GiftBoxProperties> map_players_to_giftbox;
std::mutex map_players_to_giftbox_mutex;
vector<AP_Gift> cur_gifts_available;
std::mutex cur_gifts_available_mutex;

// PRIV Func Declarations Start
AP_RequestStatus sendGiftInternal(const AP_Gift& gift);
AP_RequestStatus rejectGiftsInternal(vector<AP_Gift>& giftsToReject);
AP_NetworkPlayer getPlayer(int team, int slot);
AP_NetworkPlayer getPlayer(int team, std::string name);
AP_GiftBoxProperties getLocalGiftBoxProperties();
bool hasOpenGiftBox(int team, std::string player);
// PRIV Func Declarations End

#define AP_PLAYER_GIFTBOX_KEY ("GiftBox;" + std::to_string(ap_player_team) + ";" + std::to_string(AP_GetPlayerID()))
#define CURRENT_GIFT_PROTOCOL_VERSION 3

AP_RequestStatus AP_SetGiftBoxProperties(AP_GiftBoxProperties props) {
    if (!gifting_supported){
        logfunc("AP: Gifting isn't enabled yet, please call AP_SetGiftingSupported(true) first");
        return AP_RequestStatus::Error;
    }
    cJSON *empty = cJSON_CreateObject();

    // Create Local Box if needed
    AP_SetServerDataRequest req_local_box;
    req_local_box.key = AP_PLAYER_GIFTBOX_KEY;
    std::string LocalGiftBoxDef_s = cJSON_Print(empty);
    req_local_box.default_value = &LocalGiftBoxDef_s;
    std::string zero = "0";
    req_local_box.operations = {{"default", &zero}};
    req_local_box.type = AP_DataType::Raw;
    req_local_box.want_reply = true;
    AP_BulkSetServerData(&req_local_box);

    // Set Properties
    cJSON *GlobalGiftBox = cJSON_CreateObject();
    cJSON *playerGiftBox = cJSON_AddObjectToObject(GlobalGiftBox, std::to_string(AP_GetPlayerID()).c_str());
    ADD_BOOL(playerGiftBox, "is_open", props.IsOpen)
    ADD_BOOL(playerGiftBox, "accepts_any_gift", props.AcceptsAnyGift)
    cJSON *desiredTraits = cJSON_AddArrayToObject(playerGiftBox, "desired_traits");

    for (std::string trait_s : props.DesiredTraits)
        APPEND_STRING(desiredTraits, trait_s.c_str())

    ADD_NUMBER(playerGiftBox, "minimum_gift_data_version", CURRENT_GIFT_PROTOCOL_VERSION)
    ADD_NUMBER(playerGiftBox, "maximum_gift_data_version", CURRENT_GIFT_PROTOCOL_VERSION)

    // Update entry in MotherBox
    AP_SetServerDataRequest req_global_box;
    req_global_box.key = "GiftBoxes;" + std::to_string(ap_player_team);
    std::string GlobalGiftBox_s = cJSON_Print(GlobalGiftBox);
    std::string DefBoxGlobal_s = "{}";
    req_global_box.operations = {{"update", &GlobalGiftBox_s}};
    req_global_box.default_value = &DefBoxGlobal_s;
    req_global_box.type = AP_DataType::Raw;
    req_global_box.want_reply = false;
    AP_BulkSetServerData(&req_global_box);
    cJSON_Delete(GlobalGiftBox);

    // Set Values
    AP_CommitServerData();
    while (req_local_box.status == AP_RequestStatus::Pending && req_global_box.status == AP_RequestStatus::Pending && AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated) {}
    if (req_global_box.status != AP_RequestStatus::Done || req_local_box.status != AP_RequestStatus::Done)
        return AP_RequestStatus::Error;
    return AP_RequestStatus::Done;
}

std::map<std::pair<int,std::string>,AP_GiftBoxProperties> AP_QueryGiftBoxes() {
    std::scoped_lock lock(map_players_to_giftbox_mutex);
    return map_players_to_giftbox;
}

// Get currently available Gifts in own gift box
vector<AP_Gift> AP_CheckGifts() {
    std::scoped_lock lock(cur_gifts_available_mutex);
    return cur_gifts_available;
}

AP_RequestStatus AP_SendGift(AP_Gift gift) {
    if (!gifting_supported){
        logfunc("AP: Gifting isn't enabled yet, please call AP_SetGiftingSupported(true) first");
        return AP_RequestStatus::Error;
    }

    if (gift.IsRefund) return AP_RequestStatus::Error;

    std::pair<int,std::string> giftReceiver = {gift.ReceiverTeam, gift.Receiver};

    if (hasOpenGiftBox(gift.ReceiverTeam, gift.Receiver)) {
        gift.SenderTeam = ap_player_team;
        gift.Sender = getPlayer(ap_player_team, AP_GetPlayerID()).name;
        return sendGiftInternal(gift);
    }

    return AP_RequestStatus::Error;
}

AP_RequestStatus AP_AcceptGift(std::string id) {
    return AP_AcceptGift(set<std::string>{ id });
}
AP_RequestStatus AP_AcceptGift(set<std::string> ids) {
    if (!gifting_supported){
        logfunc("AP: Gifting isn't enabled yet, please call AP_SetGiftingSupported(true) first");
        return AP_RequestStatus::Error;
    }

   if (ids.size() == 0)
        return AP_RequestStatus::Done;

    AP_SetServerDataRequest req;
    req.key = AP_PLAYER_GIFTBOX_KEY;
    req.type = AP_DataType::Raw;
    req.want_reply = true;
    req.operations = vector<AP_DataStorageOperation>();

    vector<std::string> id_strings;
    for (std::string id : ids) 
        id_strings.push_back("\"" + id + "\"");
    for (size_t i=0; i < id_strings.size(); i++)
        req.operations.push_back({"pop", &id_strings[i]});
    
    AP_SetServerData(&req);
    while (req.status == AP_RequestStatus::Pending && AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated) {}
    return req.status;
}

AP_RequestStatus AP_RejectGift(std::string id) {
    return AP_RejectGift(set<std::string>{ id });
}
AP_RequestStatus AP_RejectGift(set<std::string> ids) {
    if (!gifting_supported){
        logfunc("AP: Gifting isn't enabled yet, please call AP_SetGiftingSupported(true) first");
        return AP_RequestStatus::Error;
    }

    vector<AP_Gift> gifts;
    vector<AP_Gift> availableGiftsCopy = AP_CheckGifts();

    for (std::string id : ids) {
        bool found = false;

        for (AP_Gift& gift : availableGiftsCopy){
            if (gift.ID == id){
                found = true;
                gifts.push_back(gift);
                break;
            }
        }

        if (!found)
            return AP_RequestStatus::Error;
    }

    return rejectGiftsInternal(gifts);
}

AP_RequestStatus rejectGiftsInternal(vector<AP_Gift>& giftsToReject){
    set<std::string> giftIds;

    for (const AP_Gift& gift : giftsToReject){
        giftIds.insert(gift.ID);
    }

    AP_RequestStatus status = AP_AcceptGift(giftIds);
    if (status == AP_RequestStatus::Error) {
        return AP_RequestStatus::Error;
    }

    bool hasError = false;
    for (AP_Gift& gift : giftsToReject) {
        gift.IsRefund = true;
        if (sendGiftInternal(gift) == AP_RequestStatus::Error)
            hasError = true;
    }

    return (hasError) ? AP_RequestStatus::Error : AP_RequestStatus::Done;
}

void AP_UseGiftAutoReject(bool enable) {
    gifting_autoReject = enable;
}

void AP_SetGiftingSupported(bool enabled){
    gifting_supported = enabled;
}

// PRIV
AP_GiftBoxProperties getLocalGiftBoxProperties(){
    std::pair<int,std::string> localGiftbox = {ap_player_team,getPlayer(ap_player_team, AP_GetPlayerID()).name};

    std::scoped_lock lock(map_players_to_giftbox_mutex);

    if (map_players_to_giftbox.count(localGiftbox) > 0)
        return map_players_to_giftbox[localGiftbox];

    AP_GiftBoxProperties uninitialized;
    uninitialized.IsOpen = false;
    uninitialized.AcceptsAnyGift = false;
    return uninitialized;
}

bool hasOpenGiftBox(int team, std::string player){
    std::pair<int,std::string> giftTarget = {team, player};

    std::scoped_lock lock(map_players_to_giftbox_mutex);
    return map_players_to_giftbox.count(giftTarget) 
        && map_players_to_giftbox[giftTarget].IsOpen == true;
}

void handleGiftAPISetReply(const AP_SetReply& reply) {
    if (reply.key == AP_PLAYER_GIFTBOX_KEY) {
        // Appears to be an array of gifts with each object being named its id
        cJSON *local_giftbox = cJSON_ParseWithLength(((std::string*)reply.value)->c_str(), ((std::string*)reply.value)->size());
        cJSON *giftId = NULL;
        std::map<std::string,AP_Gift> gifts;
        set<std::string> invalid_gifts;
        cJSON_ArrayForEach(giftId, local_giftbox) {
            AP_Gift gift;
            gift.ID = giftId->valuestring;
            /// Finish migrating...


            if (!cJSON_IsObject(giftId)) {
                invalid_gifts.insert(giftId->string);
                continue;
            }
            int sender_team = cJSON_GetObjectItem(giftId,"sender_team")->valueint;
            int sender_slot = cJSON_GetObjectItem(giftId,"sender_slot")->valueint;
            int receiver_team = cJSON_GetObjectItem(giftId,"receiver_team")->valueint;
            int receiver_slot = cJSON_GetObjectItem(giftId,"receiver_slot")->valueint;
            if (sender_team == -1 || sender_slot == -1 || receiver_team == -1 || receiver_slot == -1) {
                invalid_gifts.insert(giftId->string);
                continue;
            }
            gift.ItemName = cJSON_GetObjectItem(giftId,"item_name")->valuestring;
            gift.Amount = cJSON_GetObjectItem(giftId,"amount")->valueint; // maybe cast to uint
            gift.ItemValue = cJSON_GetObjectItem(giftId,"item_value")->valueint; //technically this value is unbounded so even uint64 isnt enough
            gift.Sender = getPlayer(sender_team, sender_slot).name;
            gift.Receiver = getPlayer(receiver_team, receiver_slot).name;
            gift.SenderTeam = sender_team;
            gift.ReceiverTeam = receiver_team;
            gift.IsRefund = cJSON_IsTrue(cJSON_GetObjectItem(giftId,"is_refund"));

            cJSON *trait = NULL;
            cJSON_ArrayForEach(trait, cJSON_GetObjectItem(giftId, "traits")) {
                if (!cJSON_IsObject(trait)) {
                    invalid_gifts.insert(giftId->string);
                    break;
                }
                AP_GiftTrait apTrait;
                apTrait.Trait = cJSON_GetObjectItem(trait, "trait")->valuestring;
                apTrait.Quality = cJSON_GetObjectItem(trait, "trait")->valuedouble;
                apTrait.Duration = cJSON_GetObjectItem(trait, "trait")->valuedouble;
                gift.Traits.push_back(apTrait);
            }

            if (!invalid_gifts.count(giftId->string))
                gifts[giftId->string] = gift;
        }

        if (!invalid_gifts.empty())
            AP_AcceptGift(invalid_gifts);

        // Perform auto-reject if giftbox closed, or traits do not match
        if (gifting_autoReject) {
            AP_GiftBoxProperties local_box_props = getLocalGiftBoxProperties();
            vector<AP_Gift> giftsToReject;
            if (!local_box_props.IsOpen) {
                for (const std::pair<std::string, AP_Gift>& gift : gifts)
                    giftsToReject.push_back(gift.second);
            } else if (!local_box_props.AcceptsAnyGift) {
                set<std::string> desired_traits(local_box_props.DesiredTraits.begin(), local_box_props.DesiredTraits.end());
                for (const std::pair<std::string, AP_Gift>& gift : gifts) {
                    bool found = false;
                    for (const AP_GiftTrait& item_trait : gift.second.Traits) {
                        if (desired_traits.count(item_trait.Trait)) {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        giftsToReject.push_back(gift.second);
                }
            }

            if (!giftsToReject.empty()){
                rejectGiftsInternal(giftsToReject);

                for (const AP_Gift gift : giftsToReject) {
                    gifts.erase(gift.ID);
                }
            }
        }

        std::scoped_lock lock(cur_gifts_available_mutex);
        cur_gifts_available.clear();
        for (const std::pair<std::string, AP_Gift>& gift : gifts) {
            cur_gifts_available.push_back(gift.second);
        }
        cJSON_Delete(local_giftbox);
    } else if (reply.key.rfind("GiftBoxes;", 0) == 0) {
        int team = std::stoi(reply.key.substr(10)); //10 = length of "GiftBoxes;"
        cJSON *json_data = cJSON_ParseWithLength((*(std::string*)reply.value).c_str(), (*(std::string*)reply.value).size());

        std::scoped_lock lock(map_players_to_giftbox_mutex);
        map_players_to_giftbox.clear();

        cJSON *motherbox = NULL;
        cJSON_ArrayForEach(motherbox, json_data) {
            int slot = std::stoi(motherbox->string);
            cJSON *player_global_props = motherbox->child;

            int min_supported_version = cJSON_GetObjectItem(player_global_props, "minimum_gift_data_version")->valueint;
            int max_supported_version = cJSON_GetObjectItem(player_global_props, "maximum_gift_data_version")->valueint;

            if (max_supported_version < CURRENT_GIFT_PROTOCOL_VERSION || min_supported_version > CURRENT_GIFT_PROTOCOL_VERSION)
                continue; //we cant send gifts to clients incompatiable with our version

            vector<std::string> DesiredTraits;
            for (unsigned int i = 0; i < cJSON_GetArraySize(cJSON_GetObjectItem(player_global_props, "desired_traits")); i++) {
                DesiredTraits.push_back(cJSON_GetArrayItem(cJSON_GetObjectItem(player_global_props, "desired_traits"), i)->valuestring);
            }

            AP_GiftBoxProperties properties;
            properties.IsOpen = cJSON_IsTrue(cJSON_GetObjectItem(player_global_props, "is_open"));
            properties.AcceptsAnyGift = cJSON_IsTrue(cJSON_GetObjectItem(player_global_props, "accepts_any_gift"));
            properties.DesiredTraits = DesiredTraits;

            map_players_to_giftbox[{team, getPlayer(team, slot).name}] = properties;
        }
        cJSON_Delete(json_data);
    }
}

AP_RequestStatus sendGiftInternal(const AP_Gift& gift) {
    if (gift.IsRefund && gift.Sender == getPlayer(ap_player_team, AP_GetPlayerID()).name) {
        // Loop detected! Rejecting a gift to yourself means you dont want what is in here. Should be safe to return success immediately
        return AP_RequestStatus::Done;
    }
    std::string giftbox_key = "GiftBox;";
    AP_NetworkPlayer SenderPlayer = getPlayer(gift.SenderTeam, gift.Sender);
    AP_NetworkPlayer ReceiverPlayer = getPlayer(gift.ReceiverTeam, gift.Receiver);
    if (!gift.IsRefund)
        giftbox_key += std::to_string(gift.ReceiverTeam) + ";" + std::to_string(ReceiverPlayer.slot);
    else
        giftbox_key += std::to_string(gift.SenderTeam) + ";" + std::to_string(SenderPlayer.slot);

    cJSON *giftVal = cJSON_CreateObject();
    if (gift.IsRefund) {
        ADD_STRING(giftVal, "id", gift.ID.c_str())
    } else {
        uint64_t random1 = rando();
        uint64_t random2 = rando();

        std::string id = "";
        id += std::to_string(random1);
        id += std::to_string(random2);
        ADD_STRING(giftVal, "id", id.c_str())
    }
    ADD_STRING(giftVal, "item_name", gift.ItemName.c_str())
    ADD_NUMBER(giftVal, "amount", gift.Amount)
    if (gift.ItemValue > 0) ADD_NUMBER(giftVal, "item_value", gift.ItemValue);
    cJSON *traitsArr = cJSON_AddArrayToObject(giftVal, "traits");
    for (const AP_GiftTrait& trait : gift.Traits) {
        cJSON *trait_v = cJSON_CreateObject();
        ADD_STRING(trait_v, "trait", trait.Trait.c_str())
        if (trait.Quality != 1.) ADD_NUMBER(trait_v, "quality", trait.Quality);
        if (trait.Duration != 1.) ADD_NUMBER(trait_v, "duration", trait.Duration);
        cJSON_AddItemToArray(traitsArr, trait_v);
    }
    ADD_NUMBER(giftVal, "sender_slot", SenderPlayer.slot)
    ADD_NUMBER(giftVal, "receiver_slot", ReceiverPlayer.slot)
    ADD_NUMBER(giftVal, "sender_team", gift.SenderTeam)
    ADD_NUMBER(giftVal, "receiver_team", gift.ReceiverTeam)
    ADD_BOOL(giftVal, "is_refund", gift.IsRefund)

    cJSON *player_box_update = cJSON_CreateObject();
    ADD_OBJECT(player_box_update, cJSON_GetObjectItem(giftVal, "id")->valuestring, giftVal)
    std::string gift_s = cJSON_Print(player_box_update);
    cJSON_Delete(player_box_update);

    AP_SetServerDataRequest req;
    req.key = giftbox_key;
    req.type = AP_DataType::Raw;
    req.want_reply = true;
    cJSON *defVal = cJSON_CreateObject();
    std::string defVal_s = cJSON_Print(defVal);
    cJSON_Delete(defVal);
    req.default_value = &defVal_s;

    req.operations = {
        {"update", &gift_s}
    };

    AP_SetServerData(&req);
    while (req.status == AP_RequestStatus::Pending && AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated) {}
    return req.status;
}
