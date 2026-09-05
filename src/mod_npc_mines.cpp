#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "GossipDef.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "Util.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

enum GossipMenuMines
{
    GOSSIP_MENU_MINES_WELCOME = 150170,
    GOSSIP_MENU_MINES_BET     = 150171,
    GOSSIP_MENU_MINES_PLAY    = 150172
};

enum MinesUiStep
{
    MINES_UI_BET  = 0,
    MINES_UI_PLAY = 1
};

enum MinesCell
{
    CELL_HIDDEN = 0,
    CELL_SAFE   = 1,
    CELL_BOMB   = 2
};

enum MinesActions
{
    ACTION_MAIN       = GOSSIP_ACTION_INFO_DEF + 1,
    ACTION_RULES      = GOSSIP_ACTION_INFO_DEF + 2,
    ACTION_PLAY       = GOSSIP_ACTION_INFO_DEF + 10,
    ACTION_BET_1      = GOSSIP_ACTION_INFO_DEF + 20,
    ACTION_BET_2      = GOSSIP_ACTION_INFO_DEF + 21,
    ACTION_BET_3      = GOSSIP_ACTION_INFO_DEF + 22,
    ACTION_BET_4      = GOSSIP_ACTION_INFO_DEF + 23,
    ACTION_BET_5      = GOSSIP_ACTION_INFO_DEF + 24,
    ACTION_CHANGE_BET = GOSSIP_ACTION_INFO_DEF + 25,
    ACTION_REFRESH    = GOSSIP_ACTION_INFO_DEF + 30,
    ACTION_PICK_CELL  = GOSSIP_ACTION_INFO_DEF + 31,
    ACTION_CASH_OUT   = GOSSIP_ACTION_INFO_DEF + 32,
    ACTION_AGAIN      = GOSSIP_ACTION_INFO_DEF + 33
};

struct MinesSession
{
    uint8  uiStep = MINES_UI_BET;
    uint64 betCopper = 0;
    uint8  bombIndex = 0;
    uint8  cells[9] = {};
    uint8  revealed = 0;
    bool   roundLive = false;
    bool   exploded = false;
    bool   cashedOut = false;
    int64  lastGold = 0;
    uint32 lastItem = 0;
    uint32 lastItemCount = 0;
    std::string lastItemName;
};

static bool MinesEnable = true;
static bool MinesAnnounce = true;
static uint64 MinesBet[5] = { 10000, 50000, 100000, 250000, 500000 };
static uint32 MinesMult[9] = { 0, 1, 2, 3, 5, 8, 12, 18, 25 };
static std::map<ObjectGuid, MinesSession> MinesSessions;
static std::string GridLetters = "ABC";

static std::string IconPath(std::string const& icon, uint32 size)
{
    return "|TInterface\\icons\\" + icon + ":" + std::to_string(size) + ":" + std::to_string(size) + "|t";
}

static std::string FormatGold(uint64 copper)
{
    return std::to_string(copper / 10000) + " oro";
}

static std::string CellIcon(uint8 state)
{
    switch (state)
    {
        case CELL_SAFE: return "INV_Box_01";
        case CELL_BOMB: return "INV_Misc_Bomb_05";
        default:        return "INV_Misc_QuestionMark";
    }
}

static MinesSession& GetSession(ObjectGuid guid)
{
    return MinesSessions[guid];
}

static uint8 PrizeQualityForRevealed(uint8 revealed)
{
    if (revealed >= 7)
        return 4;
    if (revealed >= 5)
        return 3;
    if (revealed >= 3)
        return 2;
    return 1;
}

static bool GiveItemFromTemplate(Player* player, uint32 itemId, uint32 count, std::string& outName)
{
    QueryResult itemRow = WorldDatabase.Query("SELECT entry, name FROM item_template WHERE entry = {}", itemId);
    if (!itemRow)
        return false;

    Field* fields = itemRow->Fetch();
    uint32 dbEntry = fields[0].Get<uint32>();
    outName = fields[1].Get<std::string>();
    if (dbEntry != itemId)
        return false;

    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
    if (!proto)
        return false;

    ItemPosCountVec dest;
    InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count);
    if (msg != EQUIP_ERR_OK)
    {
        player->SendEquipError(msg, nullptr, nullptr, itemId);
        return false;
    }

    Item* item = player->StoreNewItem(dest, itemId, true);
    if (!item)
        return false;

    player->SendNewItem(item, count, true, false);
    return true;
}

static bool RollItemPrize(Player* player, uint8 revealed, uint32& itemId, uint32& itemCount, std::string& itemName)
{
    uint8 quality = PrizeQualityForRevealed(revealed);
    uint32 playerLevel = player->GetLevel();

    for (uint8 attempt = 0; attempt < 10; ++attempt)
    {
        uint8 q = quality;
        if (attempt > 3 && q > 1)
            q = quality - 1;
        if (attempt > 6)
            q = 1;

        QueryResult result = WorldDatabase.Query(
            "SELECT entry, name, `class` FROM item_template "
            "WHERE Quality = {} "
            "AND `class` NOT IN (6, 10, 11, 12, 13) "
            "AND (Flags & 16) = 0 "
            "AND RequiredLevel <= {} "
            "AND name NOT LIKE '%DEPRECATED%' "
            "AND name NOT LIKE '%Test%' "
            "AND name NOT LIKE '%OLD%' "
            "AND name NOT LIKE '%NPC%' "
            "ORDER BY RAND() LIMIT 1",
            q, playerLevel);

        if (!result)
            continue;

        Field* fields = result->Fetch();
        itemId = fields[0].Get<uint32>();
        itemName = fields[1].Get<std::string>();
        uint32 itemClass = fields[2].Get<uint32>();
        itemCount = (itemClass == 7) ? urand(1, 5) : 1;

        if (GiveItemFromTemplate(player, itemId, itemCount, itemName))
            return true;
    }

    itemId = 0;
    itemCount = 0;
    itemName.clear();
    return false;
}

class npc_mines : public CreatureScript
{
public:
    npc_mines() : CreatureScript("npc_mines") { }

    static void ShowMain(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, IconPath("INV_Misc_Bomb_05", 32) + "  Jugar", GOSSIP_SENDER_MAIN, ACTION_PLAY);
        AddGossipItemFor(player, GOSSIP_ICON_TRAINER, "Como se juega", GOSSIP_SENDER_MAIN, ACTION_RULES);
        SendGossipMenuFor(player, GOSSIP_MENU_MINES_WELCOME, creature->GetGUID());
    }

    static void ShowBetPage(Player* player, Creature* creature)
    {
        player->PlayerTalkClass->ClearMenus();
        AddGossipItemFor(player, GOSSIP_ICON_VENDOR, "Elige apuesta", GOSSIP_SENDER_MAIN, ACTION_CHANGE_BET);
        for (uint8 i = 0; i < 5; ++i)
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, FormatGold(MinesBet[i]), GOSSIP_SENDER_MAIN, ACTION_BET_1 + i);
        AddGossipItemFor(player, GOSSIP_ICON_TALK, "Menu principal", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        SendGossipMenuFor(player, GOSSIP_MENU_MINES_BET, creature->GetGUID());
    }

    static void DrawBoard(Player* player, MinesSession const& session, uint32 action)
    {
        for (uint8 row = 0; row < 3; ++row)
        {
            std::string line = " ";
            for (uint8 col = 0; col < 3; ++col)
                line += IconPath(CellIcon(session.cells[row * 3 + col]), 56) + " ";
            AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, line, GOSSIP_SENDER_MAIN, action);
        }
    }

    static void ShowBoard(Player* player, Creature* creature)
    {
        MinesSession& session = GetSession(player->GetGUID());
        player->PlayerTalkClass->ClearMenus();
        DrawBoard(player, session, ACTION_REFRESH);

        if (session.roundLive)
        {
            AddGossipItemFor(player, GOSSIP_ICON_VENDOR, "Apuesta: " + FormatGold(session.betCopper) + "  |  Limpias: " + std::to_string(session.revealed), GOSSIP_SENDER_MAIN, ACTION_REFRESH);
            if (session.revealed > 0)
                AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Cobrar ahora (" + std::to_string(MinesMult[session.revealed]) + "x + item)", GOSSIP_SENDER_MAIN, ACTION_CASH_OUT);
            AddGossipItemFor(player, GOSSIP_ICON_TALK, "Destapar casilla (A1-C3)", GOSSIP_SENDER_MAIN, ACTION_PICK_CELL, "Escribe una casilla: A1, B2, C3...", 0, true);
        }
        else if (session.exploded)
        {
            AddGossipItemFor(player, GOSSIP_ICON_TABARD, "Bomba. Perdiste " + FormatGold(session.betCopper) + ".", GOSSIP_SENDER_MAIN, ACTION_REFRESH);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Otra ronda", GOSSIP_SENDER_MAIN, ACTION_AGAIN);
            AddGossipItemFor(player, GOSSIP_ICON_TALK, "Cambiar apuesta", GOSSIP_SENDER_MAIN, ACTION_CHANGE_BET);
        }
        else if (session.cashedOut)
        {
            std::string prize = "Premio: " + FormatGold(session.lastGold);
            if (session.lastItem)
                prize += " + " + session.lastItemName + " x" + std::to_string(session.lastItemCount);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, prize, GOSSIP_SENDER_MAIN, ACTION_REFRESH);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "Otra ronda", GOSSIP_SENDER_MAIN, ACTION_AGAIN);
            AddGossipItemFor(player, GOSSIP_ICON_TALK, "Cambiar apuesta", GOSSIP_SENDER_MAIN, ACTION_CHANGE_BET);
        }

        AddGossipItemFor(player, GOSSIP_ICON_TALK, "Menu principal", GOSSIP_SENDER_MAIN, ACTION_MAIN);
        SendGossipMenuFor(player, GOSSIP_MENU_MINES_PLAY, creature->GetGUID());
    }

    static void ShowPlay(Player* player, Creature* creature)
    {
        MinesSession& session = GetSession(player->GetGUID());
        if (session.uiStep == MINES_UI_BET)
        {
            ShowBetPage(player, creature);
            return;
        }
        ShowBoard(player, creature);
    }

    static void StartRound(Player* player, Creature* creature)
    {
        MinesSession& session = GetSession(player->GetGUID());
        if (!session.betCopper)
        {
            ChatHandler(player->GetSession()).SendNotification("Primero elige una apuesta.");
            ShowBetPage(player, creature);
            return;
        }

        if (player->GetMoney() < session.betCopper)
        {
            ChatHandler(player->GetSession()).SendNotification("No tienes suficiente oro.");
            ShowBetPage(player, creature);
            return;
        }

        player->ModifyMoney(-static_cast<int32>(session.betCopper));
        session.roundLive = true;
        session.exploded = false;
        session.cashedOut = false;
        session.revealed = 0;
        session.lastGold = 0;
        session.lastItem = 0;
        session.lastItemCount = 0;
        session.lastItemName.clear();
        session.bombIndex = static_cast<uint8>(urand(0, 8));
        for (uint8 i = 0; i < 9; ++i)
            session.cells[i] = CELL_HIDDEN;
        session.uiStep = MINES_UI_PLAY;
        ShowBoard(player, creature);
    }

    static void RevealAll(MinesSession& session)
    {
        for (uint8 i = 0; i < 9; ++i)
        {
            if (i == session.bombIndex)
                session.cells[i] = CELL_BOMB;
            else if (session.cells[i] == CELL_HIDDEN)
                session.cells[i] = CELL_SAFE;
        }
    }

    static void PickCell(Player* player, Creature* creature, uint8 index)
    {
        MinesSession& session = GetSession(player->GetGUID());
        if (!session.roundLive || index > 8)
        {
            ShowBoard(player, creature);
            return;
        }

        if (session.cells[index] != CELL_HIDDEN)
        {
            ChatHandler(player->GetSession()).SendNotification("Esa casilla ya esta abierta.");
            ShowBoard(player, creature);
            return;
        }

        if (index == session.bombIndex)
        {
            session.cells[index] = CELL_BOMB;
            session.roundLive = false;
            session.exploded = true;
            RevealAll(session);
            ChatHandler(player->GetSession()).SendSysMessage("Bomba. La casa se queda tu apuesta.");
            creature->HandleEmoteCommand(EMOTE_ONESHOT_QUESTION);
            ShowBoard(player, creature);
            return;
        }

        session.cells[index] = CELL_SAFE;
        session.revealed++;
        if (session.revealed >= 8)
        {
            RevealAll(session);
            CashOut(player, creature);
            return;
        }

        ShowBoard(player, creature);
    }

    static void CashOut(Player* player, Creature* creature)
    {
        MinesSession& session = GetSession(player->GetGUID());
        if (!session.roundLive || session.revealed == 0)
        {
            ShowBoard(player, creature);
            return;
        }

        session.roundLive = false;
        session.cashedOut = true;
        uint32 mult = MinesMult[session.revealed];
        uint64 gold = session.betCopper * mult;
        session.lastGold = static_cast<int64>(gold);
        player->ModifyMoney(static_cast<int32>(gold));
        player->CastSpell(player, 47292, true);
        creature->HandleEmoteCommand(EMOTE_ONESHOT_APPLAUD);

        if (RollItemPrize(player, session.revealed, session.lastItem, session.lastItemCount, session.lastItemName))
        {
            std::ostringstream msg;
            msg << "Minas: " << FormatGold(gold) << " y " << session.lastItemName << " x" << session.lastItemCount << ".";
            ChatHandler(player->GetSession()).SendSysMessage(msg.str().c_str());
        }
        else
        {
            session.lastItem = 0;
            ChatHandler(player->GetSession()).SendSysMessage(("Minas: cobraste " + FormatGold(gold) + ".").c_str());
        }

        ShowBoard(player, creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!MinesEnable)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        ShowMain(player, creature);
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (!player || !creature || sender != GOSSIP_SENDER_MAIN)
            return false;

        MinesSession& session = GetSession(player->GetGUID());

        switch (action)
        {
            case ACTION_MAIN:
                ShowMain(player, creature);
                break;
            case ACTION_RULES:
            {
                player->PlayerTalkClass->ClearMenus();
                AddGossipItemFor(player, GOSSIP_ICON_TALK, "Rejilla 3x3. Una bomba. Destapa tesoros (A1-C3) y cobra cuando quieras. Mas casillas limpias = mas oro y un item aleatorio de item_template (mejor calidad si destapas mas).", GOSSIP_SENDER_MAIN, ACTION_RULES);
                AddGossipItemFor(player, GOSSIP_ICON_TALK, "Menu principal", GOSSIP_SENDER_MAIN, ACTION_MAIN);
                SendGossipMenuFor(player, GOSSIP_MENU_MINES_WELCOME, creature->GetGUID());
                break;
            }
            case ACTION_PLAY:
                session.uiStep = MINES_UI_BET;
                session.roundLive = false;
                ShowPlay(player, creature);
                break;
            case ACTION_BET_1:
            case ACTION_BET_2:
            case ACTION_BET_3:
            case ACTION_BET_4:
            case ACTION_BET_5:
                session.betCopper = MinesBet[action - ACTION_BET_1];
                StartRound(player, creature);
                break;
            case ACTION_CHANGE_BET:
                session.roundLive = false;
                session.uiStep = MINES_UI_BET;
                ShowBetPage(player, creature);
                break;
            case ACTION_REFRESH:
                ShowPlay(player, creature);
                break;
            case ACTION_CASH_OUT:
                CashOut(player, creature);
                break;
            case ACTION_AGAIN:
                StartRound(player, creature);
                break;
            default:
                ShowMain(player, creature);
                break;
        }
        return true;
    }

    bool OnGossipSelectCode(Player* player, Creature* creature, uint32 sender, uint32 action, char const* code) override
    {
        if (!player || !creature || sender != GOSSIP_SENDER_MAIN || action != ACTION_PICK_CELL)
        {
            CloseGossipMenuFor(player);
            return true;
        }

        std::string cell = code ? std::string(code) : "";
        Utf8ToUpperOnlyLatin(cell);
        if (cell.size() != 2 || GridLetters.find(cell[0]) == std::string::npos || cell[1] < '1' || cell[1] > '3')
        {
            ChatHandler(player->GetSession()).SendNotification("Usa A1, A2, A3, B1, B2, B3, C1, C2 o C3.");
            ShowBoard(player, creature);
            return true;
        }

        uint8 index = static_cast<uint8>((GridLetters.find(cell[0]) * 3) + (cell[1] - '1'));
        PickCell(player, creature, index);
        return true;
    }
};

class MinesWorld : public WorldScript
{
public:
    MinesWorld() : WorldScript("MinesWorld", {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD
    }) { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        MinesEnable = sConfigMgr->GetOption<bool>("Mines.Enable", true);
        MinesAnnounce = sConfigMgr->GetOption<bool>("Mines.Announce", true);
        MinesBet[0] = sConfigMgr->GetOption<uint64>("Mines.Bet1", 10000);
        MinesBet[1] = sConfigMgr->GetOption<uint64>("Mines.Bet2", 50000);
        MinesBet[2] = sConfigMgr->GetOption<uint64>("Mines.Bet3", 100000);
        MinesBet[3] = sConfigMgr->GetOption<uint64>("Mines.Bet4", 250000);
        MinesBet[4] = sConfigMgr->GetOption<uint64>("Mines.Bet5", 500000);
        MinesMult[1] = sConfigMgr->GetOption<uint32>("Mines.Mult1", 1);
        MinesMult[2] = sConfigMgr->GetOption<uint32>("Mines.Mult2", 2);
        MinesMult[3] = sConfigMgr->GetOption<uint32>("Mines.Mult3", 3);
        MinesMult[4] = sConfigMgr->GetOption<uint32>("Mines.Mult4", 5);
        MinesMult[5] = sConfigMgr->GetOption<uint32>("Mines.Mult5", 8);
        MinesMult[6] = sConfigMgr->GetOption<uint32>("Mines.Mult6", 12);
        MinesMult[7] = sConfigMgr->GetOption<uint32>("Mines.Mult7", 18);
        MinesMult[8] = sConfigMgr->GetOption<uint32>("Mines.Mult8", 25);
    }
};

class MinesAnnounceScript : public PlayerScript
{
public:
    MinesAnnounceScript() : PlayerScript("MinesAnnounceScript", {
        PLAYERHOOK_ON_LOGIN
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (MinesEnable && MinesAnnounce)
            ChatHandler(player->GetSession()).SendSysMessage("Este servidor tiene el modulo |cff4CFF00Campo de minas|r.");
    }
};

void AddNpcMinesScripts()
{
    new MinesWorld();
    new MinesAnnounceScript();
    new npc_mines();
}
