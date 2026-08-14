/*
 * This file is part of mod-rndbot-sync.
 * Copyright (C) 2026 Yuof
 *
 * Derived from mod-player-bot-level-brackets (AGPL-3.0).
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

// Core AzerothCore headers
#include "mod-rndbot-sync.h"
#include "ScriptMgr.h"
#include "Log.h"
#include "Configuration/Config.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "WorldSession.h"
#include "ObjectAccessor.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "ArenaTeamMgr.h"
#include "World.h"
#include "Random.h"

// mod-playerbots headers
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Item.h"
#include "Bag.h"
#include "ItemTemplate.h"

// Standard library
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <cctype>
#include <cmath>

using namespace Acore::ChatCommands;

// ---- Configuration -----------------------------------------------------------
static bool   g_Enabled = true;
static uint32 g_CheckFrequency = 300;              // seconds; 0 = events only
static uint32 g_TargetPercent = 30;                // % of eligible bots in band
static uint32 g_TargetBand = 3;                    // +/- levels around target
static uint32 g_ProcessLimit = 5;                  // max adjustments per pass; 0 = all
static bool   g_AllowDownleveling = true;          // pull bots above the ceiling down
static bool   g_IgnoreGuildBotsWithRealPlayers = true;
static bool   g_IgnoreFriendListed = true;
static bool   g_IgnoreArenaTeamBots = true;
static bool   g_IgnoreGroupedBots = true;          // bots in any party/raid
static std::vector<std::string> g_ExcludeBotNames;
static uint32 g_GuildCacheRefreshFrequency = 600;  // seconds
static bool   g_DkProgressionEnabled = false;
static uint32 g_DkRequiredState = 13;              // PROGRESSION_TBC_TIER_5
static bool   g_FullDebug = false;
static bool   g_LiteDebug = false;
static bool  g_IlvlSyncEnabled = true;      // cap bot gear at the player's ilvl
static int32 g_OriginalGearScoreLimit = 0;  // configured AiPlayerbot.RandomGearScoreLimit

// Pulled from mod-playerbots config.
static uint8  g_RandomBotMinLevel = 1;
static uint8  g_RandomBotMaxLevel = 80;
static std::string g_RandomBotAccountPrefix = "rndbot";

static void LoadConfig()
{
    g_Enabled = sConfigMgr->GetOption<bool>("RndBotSync.Enabled", true);
    g_CheckFrequency = sConfigMgr->GetOption<uint32>("RndBotSync.CheckFrequency", 300);
    g_TargetPercent = sConfigMgr->GetOption<uint32>("RndBotSync.TargetPercent", 30);
    g_TargetBand = sConfigMgr->GetOption<uint32>("RndBotSync.TargetBand", 3);
    g_ProcessLimit = sConfigMgr->GetOption<uint32>("RndBotSync.ProcessLimit", 5);
    g_AllowDownleveling = sConfigMgr->GetOption<bool>("RndBotSync.AllowDownleveling", true);
    g_IgnoreGuildBotsWithRealPlayers = sConfigMgr->GetOption<bool>("RndBotSync.IgnoreGuildBotsWithRealPlayers", true);
    g_IgnoreFriendListed = sConfigMgr->GetOption<bool>("RndBotSync.IgnoreFriendListed", true);
    g_IgnoreArenaTeamBots = sConfigMgr->GetOption<bool>("RndBotSync.IgnoreArenaTeamBots", true);
    g_IgnoreGroupedBots = sConfigMgr->GetOption<bool>("RndBotSync.IgnoreGroupedBots", true);
    g_GuildCacheRefreshFrequency = sConfigMgr->GetOption<uint32>("RndBotSync.GuildCacheRefreshFrequency", 600);
    g_DkProgressionEnabled = sConfigMgr->GetOption<bool>("RndBotSync.DeathKnightProgression.Enabled", false);
    g_DkRequiredState = sConfigMgr->GetOption<uint32>("RndBotSync.DeathKnightProgression.RequiredState", 13);
    g_IlvlSyncEnabled = sConfigMgr->GetOption<bool>("RndBotSync.IlvlSync.Enabled", true);
    // Read directly from config so it is order-independent of mod-playerbots and
    // reflects the user's configured value (the cap we restore to when off).
    g_OriginalGearScoreLimit = sConfigMgr->GetOption<int32>("AiPlayerbot.RandomGearScoreLimit", 0);
    g_FullDebug = sConfigMgr->GetOption<bool>("RndBotSync.FullDebugMode", false);
    g_LiteDebug = sConfigMgr->GetOption<bool>("RndBotSync.LiteDebugMode", false);

    g_RandomBotMinLevel = static_cast<uint8>(sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotMinLevel", 1));
    g_RandomBotMaxLevel = static_cast<uint8>(sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotMaxLevel", 80));
    g_RandomBotAccountPrefix = sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot");

    // mod-playerbots ships its own random-bot re-leveling (level brackets, level reset). Running
    // either alongside this module means two systems fighting over every bot's level, so stand
    // down. Read straight from config so this is order-independent of mod-playerbots' own init.
    for (char const* option : { "AiPlayerbot.LevelBrackets.Enabled", "AiPlayerbot.ResetBotLevel.Enabled" })
    {
        if (g_Enabled && sConfigMgr->GetOption<bool>(option, false))
        {
            LOG_ERROR("server.loading",
                      "[RndBotSync] Disabling: {} is enabled. mod-playerbots re-levels random bots itself; "
                      "running both fights over every bot's level. Set one of the two to 0.", option);
            g_Enabled = false;
        }
    }

    std::string excludeNames = sConfigMgr->GetOption<std::string>("RndBotSync.ExcludeNames", "");
    g_ExcludeBotNames.clear();
    std::istringstream stream(excludeNames);
    std::string name;
    while (std::getline(stream, name, ','))
    {
        name.erase(std::remove_if(name.begin(), name.end(), ::isspace), name.end());
        if (!name.empty())
        {
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            g_ExcludeBotNames.push_back(name);
        }
    }
}

// ---- Identity ----------------------------------------------------------------
static std::vector<uint64> g_SocialFriendsList;

static bool IsPlayerRandomBot(Player* player)
{
    return player && sRandomPlayerbotMgr.IsRandomBot(player);
}

// A real (human) player, as opposed to any mod-playerbots bot. Reads the bot
// flag set on the session at creation, so it is reliable even before a bot's
// PlayerbotAI is registered (which only happens after the login hook fires).
//
// Deliberately not mod-playerbots' own ::IsRealPlayer (PlayerbotAI.h): that one
// tests !GET_PLAYERBOT_AI(player), which reports bots as human during
// OnPlayerLogin and would let a bot become the level target. Named differently
// so this never collides with that declaration again.
static bool IsHumanPlayer(Player* player)
{
    return player && player->GetSession() && !player->GetSession()->IsBot();
}

// ---- Exclusions --------------------------------------------------------------
static void LoadSocialFriendList()
{
    g_SocialFriendsList.clear();
    QueryResult result = CharacterDatabase.Query("SELECT friend FROM character_social WHERE flags = 1");
    if (!result)
    {
        return;
    }
    do
    {
        g_SocialFriendsList.push_back(result->Fetch()->Get<uint32>());
    } while (result->NextRow());
}

static bool BotInFriendList(Player* bot)
{
    if (!bot)
    {
        return false;
    }
    uint64 raw = bot->GetGUID().GetRawValue();
    for (uint64 friendGuid : g_SocialFriendsList)
    {
        if (friendGuid == raw)
        {
            return true;
        }
    }
    return false;
}

static bool BotInArenaTeam(Player* bot)
{
    if (!bot)
    {
        return false;
    }
    for (uint32 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
    {
        if (sArenaTeamMgr->GetArenaTeamById(bot->GetArenaTeamId(slot)))
        {
            return true;
        }
    }
    return false;
}

static bool BotNameExcluded(Player* bot)
{
    if (g_ExcludeBotNames.empty())
    {
        return false;
    }
    std::string name = bot->GetName();
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    return std::find(g_ExcludeBotNames.begin(), g_ExcludeBotNames.end(), name) != g_ExcludeBotNames.end();
}

// ---- Real-player accounts / guild cache --------------------------------------
static std::unordered_set<uint32> g_RealPlayerGuildIds;

// Builds a comma-separated list of account ids that are NOT random-bot accounts
// (username does not start with the configured prefix). Returns false if the
// prefix is empty or there are no such accounts.
static bool GetRealPlayerAccountIds(std::string& accountIdsOut)
{
    accountIdsOut.clear();
    if (g_RandomBotAccountPrefix.empty())
    {
        LOG_WARN("server.loading", "[RndBotSync] AiPlayerbot.RandomBotAccountPrefix is empty; cannot identify real accounts.");
        return false;
    }

    uint32 prefixLength = static_cast<uint32>(g_RandomBotAccountPrefix.size());
    std::string prefix = g_RandomBotAccountPrefix;
    LoginDatabase.EscapeString(prefix);

    QueryResult result = LoginDatabase.Query("SELECT id FROM account WHERE LEFT(username, {}) <> '{}'", prefixLength, prefix);
    if (!result)
    {
        return false;
    }
    do
    {
        if (!accountIdsOut.empty())
        {
            accountIdsOut += ',';
        }
        accountIdsOut += std::to_string(result->Fetch()->Get<uint32>());
    } while (result->NextRow());

    return !accountIdsOut.empty();
}

// Rebuilds the set of guild ids that have at least one real-player member
// (online or offline). Membership-based, so offline guildmates still count.
static void RefreshRealPlayerGuildCache()
{
    g_RealPlayerGuildIds.clear();

    std::string accountIds;
    if (!GetRealPlayerAccountIds(accountIds))
    {
        return;
    }

    QueryResult result = CharacterDatabase.Query(
        "SELECT DISTINCT gm.guildid FROM guild_member gm "
        "JOIN characters c ON c.guid = gm.guid WHERE c.account IN ({})",
        accountIds);
    if (!result)
    {
        return;
    }
    do
    {
        g_RealPlayerGuildIds.insert(result->Fetch()->Get<uint32>());
    } while (result->NextRow());

    if (g_FullDebug || g_LiteDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Real-player guild cache: {} guild(s).", g_RealPlayerGuildIds.size());
    }
}

static bool BotInRealPlayerGuild(Player* bot)
{
    if (!bot)
    {
        return false;
    }
    uint32 guildId = bot->GetGuildId();
    return guildId != 0 && g_RealPlayerGuildIds.count(guildId) > 0;
}

// ---- Death Knight progression gate ------------------------------------------
// Sticky for the session: once unlocked, never re-locks.
static bool g_DkUnlocked = false;

static void EvaluateDeathKnightGate()
{
    if (!g_DkProgressionEnabled)
    {
        return; // Leave mod-playerbots' own DisableDeathKnightLogin setting alone.
    }

    if (g_DkUnlocked)
    {
        sPlayerbotAIConfig.disableDeathKnightLogin = false;
        return;
    }

    std::string accountIds;
    if (!GetRealPlayerAccountIds(accountIds))
    {
        sPlayerbotAIConfig.disableDeathKnightLogin = true;
        return;
    }

    uint32 requiredQuest = 66000 + g_DkRequiredState;
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM character_queststatus_rewarded qr "
        "JOIN characters c ON c.guid = qr.guid "
        "WHERE qr.quest = {} AND c.account IN ({}) LIMIT 1",
        requiredQuest, accountIds);

    if (result)
    {
        g_DkUnlocked = true;
        sPlayerbotAIConfig.disableDeathKnightLogin = false;
        if (g_FullDebug || g_LiteDebug)
        {
            LOG_INFO("server.loading", "[RndBotSync] DK gate met (state {}); Death Knight bots unlocked.", g_DkRequiredState);
        }
    }
    else
    {
        sPlayerbotAIConfig.disableDeathKnightLogin = true;
        if (g_FullDebug)
        {
            LOG_INFO("server.loading", "[RndBotSync] DK gate not met (state {}); Death Knight bots blocked.", g_DkRequiredState);
        }
    }
}

// True if this bot must NOT be adjusted.
static bool IsBotExcluded(Player* bot)
{
    if (g_IgnoreGuildBotsWithRealPlayers && BotInRealPlayerGuild(bot))
    {
        return true;
    }
    if (g_IgnoreFriendListed && BotInFriendList(bot))
    {
        return true;
    }
    if (g_IgnoreArenaTeamBots && BotInArenaTeam(bot))
    {
        return true;
    }
    if (g_IgnoreGroupedBots && bot && bot->GetGroup())
    {
        return true;
    }
    if (BotNameExcluded(bot))
    {
        return true;
    }
    return false;
}

// Average of the best owned item level per equipment slot (equipped + carried
// bags; bank excluded). Robust to transient situational equips: a real weapon
// sitting in a bag still wins the main-hand slot over an equipped fishing pole.
// Returns 0 if the player owns nothing equippable.
static uint32 GetBestOwnedItemLevel(Player* player)
{
    if (!player)
    {
        return 0;
    }

    uint32 bestPerSlot[EQUIPMENT_SLOT_END] = { 0 };

    auto consider = [&](Item* item)
    {
        if (!item)
        {
            return;
        }
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
        {
            return;
        }
        // Small isEquipable check: only weapons/armor that this player can slot.
        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
        {
            return;
        }
        uint8 slot = player->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot >= EQUIPMENT_SLOT_END)
        {
            return; // NULL_SLOT / not equippable by this player
        }
        if (proto->ItemLevel > bestPerSlot[slot])
        {
            bestPerSlot[slot] = proto->ItemLevel;
        }
    };

    // Equipped slots.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    }
    // Backpack.
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    }
    // Equipped bags.
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag* pBag = player->GetBagByPos(bag);
        if (!pBag)
        {
            continue;
        }
        for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
        {
            consider(player->GetItemByPos(bag, static_cast<uint8>(j)));
        }
    }

    uint32 sum = 0;
    uint32 count = 0;
    for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (bestPerSlot[slot] > 0)
        {
            sum += bestPerSlot[slot];
            ++count;
        }
    }
    return count ? sum / count : 0;
}

// Sets or restores the global gear-score cap so mod-playerbots caps all bot gear
// (default bot init and re-randomize) at the target player's item level. Same
// runtime-override pattern as the DK feature's disableDeathKnightLogin.
static void UpdateGlobalGearCap(Player* targetPlayer)
{
    if (!g_IlvlSyncEnabled || !targetPlayer)
    {
        sPlayerbotAIConfig.randomGearScoreLimit = g_OriginalGearScoreLimit;
        return;
    }

    uint32 ilvl = GetBestOwnedItemLevel(targetPlayer);
    if (ilvl == 0)
    {
        // Player owns nothing equippable: don't set 0 (mod-playerbots treats 0 as
        // "unlimited"); leave the configured cap in place.
        sPlayerbotAIConfig.randomGearScoreLimit = g_OriginalGearScoreLimit;
        return;
    }

    // Clamp to >= 1 so the cap is never the "unlimited" sentinel.
    sPlayerbotAIConfig.randomGearScoreLimit = std::max<int32>(1, static_cast<int32>(ilvl));

    if (g_FullDebug || g_LiteDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Gear cap set to item level {} (player '{}').",
                 sPlayerbotAIConfig.randomGearScoreLimit, targetPlayer->GetName());
    }
}

// ---- Adjustment pass ---------------------------------------------------------

// True if the bot's transient state allows a level change right now. Persistent
// exclusions are handled separately by IsBotExcluded. Caller guarantees a
// non-null, in-world bot.
static bool IsBotSafeToAdjust(Player* bot)
{
    if (!bot->IsAlive())
    {
        return false;
    }
    if (bot->IsInCombat())
    {
        return false;
    }
    if (bot->InBattleground() || bot->InArena() || bot->inRandomLfgDungeon() || bot->InBattlegroundQueue())
    {
        return false;
    }
    if (bot->IsInFlight())
    {
        return false;
    }
    return true;
}

// Sets a single bot's level to a random value in [lower, upper] and re-randomizes
// gear/stats. Used both to raise (promote) and lower (downlevel) a bot. Death
// Knights are floored at the heroic start level; if the range is entirely below
// it the bot is skipped. Returns false (no change) for bots that are logging out or not safe
// to change right now (dead, in combat, in an instance/queue, in flight).
static bool SetBotLevelInRange(Player* bot, int lower, int upper)
{
    if (!bot || !bot->IsInWorld() || !bot->GetSession() ||
        bot->GetSession()->isLogingOut() || bot->IsDuringRemoveFromWorld())
    {
        return false;
    }

    if (!IsBotSafeToAdjust(bot))
    {
        return false;
    }

    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        int const dkMinLevel = static_cast<int>(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
        if (upper < dkMinLevel)
        {
            return false;
        }
        if (lower < dkMinLevel)
        {
            lower = dkMinLevel;
        }
    }
    if (lower > upper)
    {
        return false;
    }

    uint8 newLevel = static_cast<uint8>(urand(lower, upper));

    if (bot->IsMounted())
    {
        bot->Dismount();
    }

    PlayerbotFactory factory(bot, newLevel);
    factory.Randomize(false);

    // Known mod-playerbots quirk: re-init talents when rolling to max level with
    // equip/spec persistence enabled.
    if (newLevel == g_RandomBotMaxLevel && sPlayerbotAIConfig.equipAndSpecPersistence)
    {
        PlayerbotFactory talentFactory(bot, newLevel);
        talentFactory.InitTalentsTree(false, true, true);
    }

    if (g_FullDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Set bot '{}' to level {}.", bot->GetName(), newLevel);
    }
    return true;
}

static void RunAdjustmentPass(bool unlimitedBudget = false)
{
    if (!g_Enabled)
    {
        return;
    }

    int target = -1;
    Player* targetPlayer = nullptr;
    std::vector<Player*> eligible;

    auto const& players = ObjectAccessor::GetPlayers();
    for (auto const& itr : players)
    {
        Player* player = itr.second;
        if (!player || !player->IsInWorld())
        {
            continue;
        }

        if (IsHumanPlayer(player))
        {
            // Real player: highest-level one is the target.
            if (static_cast<int>(player->GetLevel()) > target)
            {
                target = static_cast<int>(player->GetLevel());
                targetPlayer = player;
            }
            continue;
        }

        if (!IsPlayerRandomBot(player) || IsBotExcluded(player))
        {
            continue;
        }
        eligible.push_back(player);
    }

    // Item level sync: set the gear cap from the target player (or restore the
    // original when none online). Independent of the level-adjustment steps.
    UpdateGlobalGearCap(targetPlayer);

    if (target < 0)
    {
        return; // No real player online: freeze.
    }
    if (g_TargetPercent == 0 && !g_AllowDownleveling)
    {
        return; // Level adjustment disabled; gear cap already handled above.
    }
    if (eligible.empty())
    {
        return;
    }

    // The band's upper edge is the ceiling: the highest real player's level plus
    // the band, treated as the effective maximum for the whole bot population.
    uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    int bandLower = target - static_cast<int>(g_TargetBand);
    if (bandLower < static_cast<int>(g_RandomBotMinLevel))
    {
        bandLower = g_RandomBotMinLevel;
    }
    int ceiling = target + static_cast<int>(g_TargetBand);
    if (ceiling > static_cast<int>(maxLevel))
    {
        ceiling = static_cast<int>(maxLevel);
    }

    bool const unlimited = unlimitedBudget || (g_ProcessLimit == 0);
    uint32 budget = g_ProcessLimit;
    uint32 downleveled = 0;
    uint32 promoted = 0;

    // ---- Step 1: enforce the ceiling (downlevel bots above it) ----
    if (g_AllowDownleveling)
    {
        std::vector<Player*> aboveCeiling;
        for (Player* bot : eligible)
        {
            if (static_cast<int>(bot->GetLevel()) > ceiling)
            {
                aboveCeiling.push_back(bot);
            }
        }
        // Highest first: bring the most out-of-place bots down first.
        std::sort(aboveCeiling.begin(), aboveCeiling.end(), [](Player* a, Player* b)
        {
            return a->GetLevel() > b->GetLevel();
        });
        for (Player* bot : aboveCeiling)
        {
            if (!unlimited && budget == 0)
            {
                break;
            }
            // Scatter across [RandomBotMinLevel, ceiling]. Unsafe bots (combat,
            // instance, flight, ...) are skipped without spending budget.
            if (SetBotLevelInRange(bot, g_RandomBotMinLevel, ceiling))
            {
                ++downleveled;
                if (!unlimited)
                {
                    --budget;
                }
            }
        }
    }

    // ---- Step 2: concentrate TargetPercent% inside the band (promote up) ----
    if (g_TargetPercent > 0 && (unlimited || budget > 0))
    {
        uint32 desiredNear = static_cast<uint32>(std::lround(g_TargetPercent / 100.0 * eligible.size()));

        uint32 currentNear = 0;
        std::vector<Player*> belowBand;
        for (Player* bot : eligible)
        {
            int level = static_cast<int>(bot->GetLevel());
            if (level >= bandLower && level <= ceiling)
            {
                ++currentNear;
            }
            else if (level < bandLower)
            {
                belowBand.push_back(bot);
            }
        }

        if (currentNear < desiredNear)
        {
            uint32 need = desiredNear - currentNear;

            // Lowest-level first, so the population moves upward into the band.
            std::sort(belowBand.begin(), belowBand.end(), [](Player* a, Player* b)
            {
                return a->GetLevel() < b->GetLevel();
            });

            for (Player* bot : belowBand)
            {
                if (need == 0 || (!unlimited && budget == 0))
                {
                    break;
                }
                // Unsafe bots are skipped without spending budget or the deficit.
                if (SetBotLevelInRange(bot, bandLower, ceiling))
                {
                    ++promoted;
                    --need;
                    if (!unlimited)
                    {
                        --budget;
                    }
                }
            }
        }
    }

    if ((g_FullDebug || g_LiteDebug) && (downleveled > 0 || promoted > 0))
    {
        LOG_INFO("server.loading", "[RndBotSync] Target {} band [{}-{}], eligible {}, downleveled {}, promoted {}.",
                 target, bandLower, ceiling, eligible.size(), downleveled, promoted);
    }
}

// ---- Cache refresh helper ----------------------------------------------------
static void RefreshCaches()
{
    LoadSocialFriendList();
    RefreshRealPlayerGuildCache();
}

// ---- WorldScript -------------------------------------------------------------
class RndBotSyncWorldScript : public WorldScript
{
public:
    RndBotSyncWorldScript()
        : WorldScript("RndBotSyncWorldScript"), m_timer(0), m_guildTimer(0) { }

    void OnStartup() override
    {
        LoadConfig();
        if (!g_Enabled)
        {
            LOG_INFO("server.loading", "[RndBotSync] Disabled via configuration.");
            return;
        }
        RefreshCaches();
        EvaluateDeathKnightGate();
        LOG_INFO("server.loading", "[RndBotSync] Loaded. CheckFrequency {}s, TargetPercent {}%, TargetBand +/-{}.",
                 g_CheckFrequency, g_TargetPercent, g_TargetBand);
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_Enabled)
        {
            return;
        }

        m_guildTimer += diff;
        if (m_guildTimer >= g_GuildCacheRefreshFrequency * 1000)
        {
            m_guildTimer = 0;
            RefreshCaches();
            if (g_DkProgressionEnabled && !g_DkUnlocked)
            {
                EvaluateDeathKnightGate();
            }
        }

        if (g_CheckFrequency == 0)
        {
            return; // Events-only mode.
        }
        m_timer += diff;
        if (m_timer >= g_CheckFrequency * 1000)
        {
            m_timer = 0;
            RunAdjustmentPass();
        }
    }

private:
    uint32 m_timer;
    uint32 m_guildTimer;
};

// ---- PlayerScript ------------------------------------------------------------
class RndBotSyncPlayerScript : public PlayerScript
{
public:
    RndBotSyncPlayerScript() : PlayerScript("RndBotSyncPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_Enabled || !IsHumanPlayer(player))
        {
            return;
        }
        if (g_DkProgressionEnabled && !g_DkUnlocked)
        {
            EvaluateDeathKnightGate();
        }
        RunAdjustmentPass();
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!g_Enabled || !IsHumanPlayer(player))
        {
            return;
        }
        RunAdjustmentPass();
    }
};

// ---- CommandScript -----------------------------------------------------------
class RndBotSyncCommandScript : public CommandScript
{
public:
    RndBotSyncCommandScript() : CommandScript("RndBotSyncCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "rndbotsync reload", HandleReload, SEC_ADMINISTRATOR, Console::Yes },
            { "rndbotsync status", HandleStatus, SEC_ADMINISTRATOR, Console::Yes },
            { "rndbotsync resync", HandleResync, SEC_ADMINISTRATOR, Console::Yes }
        };
        return table;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadConfig();
        RefreshCaches();
        EvaluateDeathKnightGate();
        UpdateGlobalGearCap(nullptr);
        handler->SendSysMessage("[RndBotSync] Config reloaded.");
        return true;
    }

    // Read-only snapshot of the current target, band, eligible-bot counts, and
    // the gear/DK state. Changes nothing.
    static bool HandleStatus(ChatHandler* handler)
    {
        int target = -1;
        Player* targetPlayer = nullptr;

        auto const& players = ObjectAccessor::GetPlayers();
        for (auto const& itr : players)
        {
            Player* player = itr.second;
            if (!player || !player->IsInWorld())
            {
                continue;
            }
            if (IsHumanPlayer(player) && static_cast<int>(player->GetLevel()) > target)
            {
                target = static_cast<int>(player->GetLevel());
                targetPlayer = player;
            }
        }

        handler->PSendSysMessage("[RndBotSync] Enabled: {}, CheckFrequency: {}s, ProcessLimit: {}.",
                                 g_Enabled, g_CheckFrequency, g_ProcessLimit);

        if (target < 0)
        {
            handler->PSendSysMessage("  Target: none online (frozen).");
        }
        else
        {
            uint32 maxLevel = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
            int bandLower = std::max(static_cast<int>(g_RandomBotMinLevel), target - static_cast<int>(g_TargetBand));
            int ceiling = std::min(static_cast<int>(maxLevel), target + static_cast<int>(g_TargetBand));

            uint32 eligible = 0, inBand = 0, aboveCeiling = 0, belowBand = 0;
            for (auto const& itr : players)
            {
                Player* bot = itr.second;
                if (!bot || !bot->IsInWorld() || IsHumanPlayer(bot))
                {
                    continue;
                }
                if (!IsPlayerRandomBot(bot) || IsBotExcluded(bot))
                {
                    continue;
                }
                ++eligible;
                int level = static_cast<int>(bot->GetLevel());
                if (level > ceiling)
                {
                    ++aboveCeiling;
                }
                else if (level < bandLower)
                {
                    ++belowBand;
                }
                else
                {
                    ++inBand;
                }
            }
            uint32 desiredNear = static_cast<uint32>(std::lround(g_TargetPercent / 100.0 * eligible));

            handler->PSendSysMessage("  Target: {} (level {}).", targetPlayer->GetName(), target);
            handler->PSendSysMessage("  Band: [{}-{}] (ceiling {}), TargetPercent {} -> desired in-band {}.",
                                     bandLower, ceiling, ceiling, g_TargetPercent, desiredNear);
            handler->PSendSysMessage("  Eligible: {} (in-band {}, above-ceiling {}, below-band {}), AllowDownleveling: {}.",
                                     eligible, inBand, aboveCeiling, belowBand, g_AllowDownleveling);
        }

        handler->PSendSysMessage("  IlvlSync: {}, gear cap: {}, target best item level: {}.",
                                 g_IlvlSyncEnabled, sPlayerbotAIConfig.randomGearScoreLimit,
                                 targetPlayer ? GetBestOwnedItemLevel(targetPlayer) : 0);
        handler->PSendSysMessage("  DK gate: {}, {} (RequiredState {}).",
                                 g_DkProgressionEnabled, g_DkUnlocked ? "unlocked" : "locked", g_DkRequiredState);
        return true;
    }

    // Forces an immediate full pass that ignores the ProcessLimit throttle
    // (processes all eligible bots at once). Still respects the level ceiling,
    // gear cap, and exclusions.
    static bool HandleResync(ChatHandler* handler)
    {
        RefreshCaches();
        EvaluateDeathKnightGate();
        RunAdjustmentPass(true);
        handler->SendSysMessage("[RndBotSync] Forced full resync complete.");
        return true;
    }
};

// ---- Registration ------------------------------------------------------------
void Addmod_rndbot_syncScripts()
{
    new RndBotSyncWorldScript();
    new RndBotSyncPlayerScript();
    new RndBotSyncCommandScript();
}
