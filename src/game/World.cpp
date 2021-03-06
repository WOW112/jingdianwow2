/*
 * This file is part of the CMaNGOS Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \file
    \ingroup world
*/

#include "World.h"
#include "Database/DatabaseEnv.h"
#include "Config/Config.h"
#include "Platform/Define.h"
#include "SystemConfig.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "Player.h"
#include "AccountMgr.h"
#include "AuctionHouseMgr.h"
#include "ObjectMgr.h"
#include "CreatureEventAIMgr.h"
#include "GuildMgr.h"
#include "SpellMgr.h"
#include "Chat.h"
#include "DBCStores.h"
#include "MassMailMgr.h"
#include "LootMgr.h"
#include "ItemEnchantmentMgr.h"
#include "MapManager.h"
#include "ScriptMgr.h"
#include "CreatureAIRegistry.h"
#include "Policies/Singleton.h"
#include "BattleGround/BattleGroundMgr.h"
#include "OutdoorPvP/OutdoorPvP.h"
#include "TemporarySummon.h"
#include "VMapFactory.h"
#include "MoveMap.h"
#include "GameEventMgr.h"
#include "PoolManager.h"
#include "Database/DatabaseImpl.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "MapPersistentStateMgr.h"
#include "WaypointManager.h"
#include "GMTicketMgr.h"
#include "Util.h"
#include "AuctionHouseBot/AuctionHouseBot.h"
#include "CharacterDatabaseCleaner.h"
#include "CreatureLinkingMgr.h"
#include "Weather.h"
#include "Language.h"
#include "extras/Mod.h"

INSTANTIATE_SINGLETON_1(World);

extern void LoadGameObjectModelList();

volatile bool World::m_stopEvent = false;
uint8 World::m_ExitCode = SHUTDOWN_EXIT_CODE;
ACE_Atomic_Op<ACE_Thread_Mutex, uint32> World::m_worldLoopCounter = 0;

float World::m_MaxVisibleDistanceOnContinents = DEFAULT_VISIBILITY_DISTANCE;
float World::m_MaxVisibleDistanceInInstances  = DEFAULT_VISIBILITY_INSTANCE;
float World::m_MaxVisibleDistanceInBG         = DEFAULT_VISIBILITY_BG;

float World::m_MaxVisibleDistanceInFlight     = DEFAULT_VISIBILITY_DISTANCE;
float World::m_VisibleUnitGreyDistance        = 0;
float World::m_VisibleObjectGreyDistance      = 0;

float  World::m_relocation_lower_limit_sq     = 10.f * 10.f;
uint32 World::m_relocation_ai_notify_delay    = 1000u;

/// World constructor
World::World()
{
    m_playerLimit = 0;
    m_allowMovement = true;
    m_ShutdownMask = 0;
    m_ShutdownTimer = 0;
    m_gameTime = time(NULL);
    m_startTime = m_gameTime;
    m_maxActiveSessionCount = 0;
    m_maxQueuedSessionCount = 0;

    m_defaultDbcLocale = LOCALE_enUS;
    m_availableDbcLocaleMask = 0;

    for (int i = 0; i < CONFIG_UINT32_VALUE_COUNT; ++i)
        m_configUint32Values[i] = 0;

    for (int i = 0; i < CONFIG_INT32_VALUE_COUNT; ++i)
        m_configInt32Values[i] = 0;

    for (int i = 0; i < CONFIG_FLOAT_VALUE_COUNT; ++i)
        m_configFloatValues[i] = 0.0f;

    for (int i = 0; i < CONFIG_BOOL_VALUE_COUNT; ++i)
        m_configBoolValues[i] = false;

    m_configForceLoadMapIds = NULL;
}

/// World destructor
World::~World()
{
    ///- Empty the kicked session set
    while (!m_sessions.empty())
    {
        // not remove from queue, prevent loading new sessions
        delete m_sessions.begin()->second;
        m_sessions.erase(m_sessions.begin());
    }

    CliCommandHolder* command = NULL;
    while (cliCmdQueue.next(command))
        delete command;

    VMAP::VMapFactory::clear();
    MMAP::MMapFactory::clear();

    delete m_configForceLoadMapIds;

    // TODO free addSessQueue
}

/// Cleanups before world stop
void World::CleanupsBeforeStop()
{
    KickAll();                                       // save and kick all players
    UpdateSessions(1);                               // real players unload required UpdateSessions call
    sBattleGroundMgr.DeleteAllBattleGrounds();       // unload battleground templates before different singletons destroyed
}

/// Find a session by its id
WorldSession* World::FindSession(uint32 id) const
{
    SessionMap::const_iterator itr = m_sessions.find(id);

    if (itr != m_sessions.end())
        return itr->second;                                 // also can return NULL for kicked session
    else
        return NULL;
}

/// Remove a given session
bool World::RemoveSession(uint32 id)
{
    ///- Find the session, kick the user, but we can't delete session at this moment to prevent iterator invalidation
    SessionMap::const_iterator itr = m_sessions.find(id);

    if (itr != m_sessions.end() && itr->second)
    {
        if (itr->second->PlayerLoading())
            return false;
        itr->second->KickPlayer();
    }

    return true;
}

void World::AddSession(WorldSession* s)
{
    addSessQueue.add(s);
}

void
World::AddSession_(WorldSession* s)
{
    MANGOS_ASSERT(s);

    // NOTE - Still there is race condition in WorldSession* being used in the Sockets

    ///- kick already loaded player with same account (if any) and remove session
    ///- if player is in loading and want to load again, return
    if (!RemoveSession(s->GetAccountId()))
    {
        s->KickPlayer();
        delete s;                                           // session not added yet in session list, so not listed in queue
        return;
    }

    // decrease session counts only at not reconnection case
    bool decrease_session = true;

    // if session already exist, prepare to it deleting at next world update
    // NOTE - KickPlayer() should be called on "old" in RemoveSession()
    {
        SessionMap::const_iterator old = m_sessions.find(s->GetAccountId());

        if (old != m_sessions.end())
        {
            // prevent decrease sessions count if session queued
            if (RemoveQueuedSession(old->second))
                decrease_session = false;
            // not remove replaced session form queue if listed
            delete old->second;
        }
    }

    m_sessions[s->GetAccountId()] = s;

    uint32 Sessions = GetActiveAndQueuedSessionCount();
    uint32 pLimit = GetPlayerAmountLimit();
    uint32 QueueSize = GetQueuedSessionCount();             // number of players in the queue

    // so we don't count the user trying to
    // login as a session and queue the socket that we are using
    if (decrease_session)
        --Sessions;

    if (pLimit > 0 && Sessions >= pLimit && s->GetSecurity() == SEC_PLAYER)
    {
        AddQueuedSession(s);
        UpdateMaxSessionCounters();
        DETAIL_LOG("PlayerQueue: Account id %u is in Queue Position (%u).", s->GetAccountId(), ++QueueSize);
        return;
    }

    // Checked for 1.12.2
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1 + 4 + 1 + 4);
    packet << uint8(AUTH_OK);
    packet << uint32(0);                                    // BillingTimeRemaining
    packet << uint8(0);                                     // BillingPlanFlags
    packet << uint32(0);                                    // BillingTimeRested
    s->SendPacket(&packet);

    UpdateMaxSessionCounters();

    // Updates the population
    if (pLimit > 0)
    {
        float popu = float(GetActiveSessionCount());        // updated number of users on the server
        popu /= pLimit;
        popu *= 2;

        static SqlStatementID id;

        SqlStatement stmt = LoginDatabase.CreateStatement(id, "UPDATE realmlist SET population = ? WHERE id = ?");
        stmt.PExecute(popu, realmID);

        DETAIL_LOG("Server Population (%f).", popu);
    }
}

int32 World::GetQueuedSessionPos(WorldSession* sess)
{
    uint32 position = 1;

    for (Queue::const_iterator iter = m_QueuedSessions.begin(); iter != m_QueuedSessions.end(); ++iter, ++position)
        if ((*iter) == sess)
            return position;

    return 0;
}

void World::AddQueuedSession(WorldSession* sess)
{
    sess->SetInQueue(true);
    m_QueuedSessions.push_back(sess);

    // [-ZERO] Possible wrong
    // The 1st SMSG_AUTH_RESPONSE needs to contain other info too.
    WorldPacket packet(SMSG_AUTH_RESPONSE, 1 + 4 + 1 + 4 + 4);
    packet << uint8(AUTH_WAIT_QUEUE);
    packet << uint32(0);                                    // BillingTimeRemaining
    packet << uint8(0);                                     // BillingPlanFlags
    packet << uint32(0);                                    // BillingTimeRested
    packet << uint32(GetQueuedSessionPos(sess));            // position in queue
    sess->SendPacket(&packet);
}

bool World::RemoveQueuedSession(WorldSession* sess)
{
    // sessions count including queued to remove (if removed_session set)
    uint32 sessions = GetActiveSessionCount();

    uint32 position = 1;
    Queue::iterator iter = m_QueuedSessions.begin();

    // search to remove and count skipped positions
    bool found = false;

    for (; iter != m_QueuedSessions.end(); ++iter, ++position)
    {
        if (*iter == sess)
        {
            sess->SetInQueue(false);
            iter = m_QueuedSessions.erase(iter);
            found = true;                                   // removing queued session
            break;
        }
    }

    // iter point to next socked after removed or end()
    // position store position of removed socket and then new position next socket after removed

    // if session not queued then we need decrease sessions count
    if (!found && sessions)
        --sessions;

    // accept first in queue
    if ((!m_playerLimit || (int32)sessions < m_playerLimit) && !m_QueuedSessions.empty())
    {
        WorldSession* pop_sess = m_QueuedSessions.front();
        pop_sess->SetInQueue(false);
        pop_sess->SendAuthWaitQue(0);
        m_QueuedSessions.pop_front();

        // update iter to point first queued socket or end() if queue is empty now
        iter = m_QueuedSessions.begin();
        position = 1;
    }

    // update position from iter to end()
    // iter point to first not updated socket, position store new position
    for (; iter != m_QueuedSessions.end(); ++iter, ++position)
        (*iter)->SendAuthWaitQue(position);

    return found;
}

/// Initialize config values
void World::LoadConfigSettings(bool reload)
{
    if (reload)
    {
        if (!sConfig.Reload())
        {
            sLog.outError("World settings reload fail: can't read settings from %s.", sConfig.GetFilename().c_str());
            return;
        }
    }

    ///- Read the version of the configuration file and warn the user in case of emptiness or mismatch
    uint32 confVersion = sConfig.GetIntDefault("ConfVersion", 0);
    if (!confVersion)
    {
        sLog.outError("*****************************************************************************");
        sLog.outError(" WARNING: mangosd.conf does not include a ConfVersion variable.");
        sLog.outError("          Your configuration file may be out of date!");
        sLog.outError("*****************************************************************************");
        Log::WaitBeforeContinueIfNeed();
    }
    else
    {
        if (confVersion < _MANGOSDCONFVERSION)
        {
            sLog.outError("*****************************************************************************");
            sLog.outError(" WARNING: Your mangosd.conf version indicates your conf file is out of date!");
            sLog.outError("          Please check for updates, as your current default values may cause");
            sLog.outError("          unexpected behavior.");
            sLog.outError("*****************************************************************************");
            Log::WaitBeforeContinueIfNeed();
        }
    }

    ///- Read the player limit and the Message of the day from the config file
    SetPlayerLimit(sConfig.GetIntDefault("PlayerLimit", DEFAULT_PLAYER_LIMIT), true);
    SetMotd(sConfig.GetStringDefault("Motd", "Welcome to the Massive Network Game Object Server."));

	// VMSS system 
    setConfig(CONFIG_BOOL_VMSS_ENABLE,                "VMSS.Enable", false); 
    setConfig(CONFIG_UINT32_VMSS_MAXTHREADBREAKS,     "VMSS.MaxThreadBreaks",5); 
    setConfig(CONFIG_UINT32_VMSS_TBREMTIME,           "VMSS.ThreadBreakRememberTime",600); 
    setConfig(CONFIG_UINT32_VMSS_MAPFREEMETHOD,       "VMSS.MapFreeMethod",1); 

    setConfig(CONFIG_UINT32_VMSS_FREEZECHECKPERIOD,   "VMSS.FreezeCheckPeriod",1000); 
    setConfig(CONFIG_UINT32_VMSS_FREEZEDETECTTIME,    "VMSS.MapFreezeDetectTime",2000);
	setConfig(CONFIG_BOOL_VMSS_TRYSKIPFIRST,          "VMSS.TrySkipFirstThreadBreak", false);
	setConfig(CONFIG_UINT32_VMSS_FORCEUNLOADDELAY,    "VMSS.ForceUnloadDelay",3000);

    ///- Read all rates from the config file
    setConfigPos(CONFIG_FLOAT_RATE_HEALTH, "Rate.Health", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_POWER_MANA, "Rate.Mana", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_POWER_RAGE_INCOME, "Rate.Rage.Income", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_POWER_RAGE_LOSS, "Rate.Rage.Loss", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_POWER_FOCUS,             "Rate.Focus", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_LOYALTY,              "Rate.Loyalty", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_POWER_ENERGY,            "Rate.Energy", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_SKILL_DISCOVERY,      "Rate.Skill.Discovery",      1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_POOR,       "Rate.Drop.Item.Poor",       1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_NORMAL,     "Rate.Drop.Item.Normal",     1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_UNCOMMON,   "Rate.Drop.Item.Uncommon",   1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_RARE,       "Rate.Drop.Item.Rare",       1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_EPIC,       "Rate.Drop.Item.Epic",       1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_LEGENDARY,  "Rate.Drop.Item.Legendary",  1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_ARTIFACT,   "Rate.Drop.Item.Artifact",   1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_REFERENCED, "Rate.Drop.Item.Referenced", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DROP_MONEY,           "Rate.Drop.Money", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_DROP_ITEM_QUEST, "Rate.Drop.Item.Quest", 1.0f);
	setConfig(CONFIG_FLOAT_RATE_PET_XP_KILL, "Rate.Pet.XP.Kill", 1.0f);
	setConfig(CONFIG_FLOAT_RATE_XP_KILL_LM, "Rate.XP.Kill_Lm", 1.0f);
	setConfig(CONFIG_FLOAT_RATE_XP_KILL_BL, "Rate.XP.Kill_Bl", 1.0f);
	setConfig(CONFIG_FLOAT_RATE_XP_QUEST_LM, "Rate.XP.Quest_Lm", 1.0f);
	setConfig(CONFIG_FLOAT_RATE_XP_QUEST_BL, "Rate.XP.Quest_Bl", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_XP_EXPLORE, "Rate.XP.Explore", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_REPUTATION_GAIN,           "Rate.Reputation.Gain", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_REPUTATION_LOWLEVEL_KILL,  "Rate.Reputation.LowLevel.Kill", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_REPUTATION_LOWLEVEL_QUEST, "Rate.Reputation.LowLevel.Quest", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE, "Rate.Creature.Normal.Damage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE, "Rate.Creature.Elite.Elite.Damage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE, "Rate.Creature.Elite.RAREELITE.Damage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE, "Rate.Creature.Elite.WORLDBOSS.Damage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE, "Rate.Creature.Elite.RARE.Damage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_ZUG, "Rate.Creature.Normal.DAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_ZUG, "Rate.Creature.Elite.Elite.DAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_ZUG, "Rate.Creature.Elite.RAREELITE.DAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_ZUG, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_ZUG, "Rate.Creature.Elite.RARE.DAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_FX, "Rate.Creature.Normal.DAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_FX, "Rate.Creature.Elite.Elite.DAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_FX, "Rate.Creature.Elite.RAREELITE.DAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_FX, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_FX, "Rate.Creature.Elite.RARE.DAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_MC, "Rate.Creature.Normal.DAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_MC, "Rate.Creature.Elite.Elite.DAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_MC, "Rate.Creature.Elite.RAREELITE.DAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_MC, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_MC, "Rate.Creature.Elite.RARE.DAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_HL, "Rate.Creature.Normal.DAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_HL, "Rate.Creature.Elite.Elite.DAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_HL, "Rate.Creature.Elite.RAREELITE.DAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_HL, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_HL, "Rate.Creature.Elite.RARE.DAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_BWL, "Rate.Creature.Normal.DAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_BWL, "Rate.Creature.Elite.Elite.DAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_BWL, "Rate.Creature.Elite.RAREELITE.DAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_BWL, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_BWL, "Rate.Creature.Elite.RARE.DAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_TAQ, "Rate.Creature.Normal.DAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_TAQ, "Rate.Creature.Elite.Elite.DAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_TAQ, "Rate.Creature.Elite.RAREELITE.DAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_TAQ, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_TAQ, "Rate.Creature.Elite.RARE.DAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_NAXX, "Rate.Creature.Normal.DAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_NAXX, "Rate.Creature.Elite.Elite.DAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_NAXX, "Rate.Creature.Elite.RAREELITE.DAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_NAXX, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_NAXX, "Rate.Creature.Elite.RARE.DAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_AS, "Rate.Creature.Normal.DAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_AS, "Rate.Creature.Elite.Elite.DAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_AS, "Rate.Creature.Elite.RAREELITE.DAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_AS, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_AS, "Rate.Creature.Elite.RARE.DAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP, "Rate.Creature.Normal.HP", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP, "Rate.Creature.Elite.Elite.HP", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP, "Rate.Creature.Elite.RAREELITE.HP", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP, "Rate.Creature.Elite.WORLDBOSS.HP", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP, "Rate.Creature.Elite.RARE.HP", 1.0f);
	//--------------5人本血量配置------------
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_DM, "Rate.Creature.Normal.HP_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_DM, "Rate.Creature.Elite.Elite.HP_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_DM, "Rate.Creature.Elite.RAREELITE.HP_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_DM, "Rate.Creature.Elite.WORLDBOSS.HP_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_DM, "Rate.Creature.Elite.RARE.HP_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_SM, "Rate.Creature.Normal.HP_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_SM, "Rate.Creature.Elite.Elite.HP_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_SM, "Rate.Creature.Elite.RAREELITE.HP_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_SM, "Rate.Creature.Elite.WORLDBOSS.HP_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_SM, "Rate.Creature.Elite.RARE.HP_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_STSM, "Rate.Creature.Normal.HP_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_STSM, "Rate.Creature.Elite.Elite.HP_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_STSM, "Rate.Creature.Elite.RAREELITE.HP_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_STSM, "Rate.Creature.Elite.WORLDBOSS.HP_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_STSM, "Rate.Creature.Elite.RARE.HP_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_BD, "Rate.Creature.Normal.HP_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_BD, "Rate.Creature.Elite.Elite.HP_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_BD, "Rate.Creature.Elite.RAREELITE.HP_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_BD, "Rate.Creature.Elite.WORLDBOSS.HP_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_BD, "Rate.Creature.Elite.RARE.HP_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_BS, "Rate.Creature.Normal.HP_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_BS, "Rate.Creature.Elite.Elite.HP_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_BS, "Rate.Creature.Elite.RAREELITE.HP_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_BS, "Rate.Creature.Elite.WORLDBOSS.HP_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_BS, "Rate.Creature.Elite.RARE.HP_BS", 1.0f);
	//-----------------------------------------
	//--------------5人本物理伤害配置------------
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_DM, "Rate.Creature.Normal.DAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_DM, "Rate.Creature.Elite.Elite.DAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_DM, "Rate.Creature.Elite.RAREELITE.DAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_DM, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_DM, "Rate.Creature.Elite.RARE.DAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_SM, "Rate.Creature.Normal.DAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_SM, "Rate.Creature.Elite.Elite.DAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_SM, "Rate.Creature.Elite.RAREELITE.DAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_SM, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_SM, "Rate.Creature.Elite.RARE.DAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_STSM, "Rate.Creature.Normal.DAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_STSM, "Rate.Creature.Elite.Elite.DAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_STSM, "Rate.Creature.Elite.RAREELITE.DAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_STSM, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_STSM, "Rate.Creature.Elite.RARE.DAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_BD, "Rate.Creature.Normal.DAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_BD, "Rate.Creature.Elite.Elite.DAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_BD, "Rate.Creature.Elite.RAREELITE.DAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_BD, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_BD, "Rate.Creature.Elite.RARE.DAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_DAMAGE_BS, "Rate.Creature.Normal.DAMAGE_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_DAMAGE_BS, "Rate.Creature.Elite.Elite.DAMAGE_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_DAMAGE_BS, "Rate.Creature.Elite.RAREELITE.DAMAGE_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_DAMAGE_BS, "Rate.Creature.Elite.WORLDBOSS.DAMAGE_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_DAMAGE_BS, "Rate.Creature.Elite.RARE.DAMAGE_BS", 1.0f);
	//------------------------------------------------
	//--------------5人本法术伤害配置------------
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_DM, "Rate.Creature.Normal.SPELLDAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_DM, "Rate.Creature.Elite.Elite.SPELLDAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_DM, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_DM, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_DM, "Rate.Creature.Elite.RARE.SPELLDAMAGE_DM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_SM, "Rate.Creature.Normal.SPELLDAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_SM, "Rate.Creature.Elite.Elite.SPELLDAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_SM, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_SM, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_SM, "Rate.Creature.Elite.RARE.SPELLDAMAGE_SM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_STSM, "Rate.Creature.Normal.SPELLDAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_STSM, "Rate.Creature.Elite.Elite.SPELLDAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_STSM, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_STSM, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_STSM, "Rate.Creature.Elite.RARE.SPELLDAMAGE_STSM", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_BD, "Rate.Creature.Normal.SPELLDAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_BD, "Rate.Creature.Elite.Elite.SPELLDAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_BD, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_BD, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_BD, "Rate.Creature.Elite.RARE.SPELLDAMAGE_BD", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_BS, "Rate.Creature.Normal.SPELLDAMAGE_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_BS, "Rate.Creature.Elite.Elite.SPELLDAMAGE_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_BS, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_BS, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_BS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_BS, "Rate.Creature.Elite.RARE.SPELLDAMAGE_BS", 1.0f);
	//--------------------------------------
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_ZUG, "Rate.Creature.Normal.HP_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_ZUG, "Rate.Creature.Elite.Elite.HP_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_ZUG, "Rate.Creature.Elite.RAREELITE.HP_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_ZUG, "Rate.Creature.Elite.WORLDBOSS.HP_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_ZUG, "Rate.Creature.Elite.RARE.HP_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_FX, "Rate.Creature.Normal.HP_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_FX, "Rate.Creature.Elite.Elite.HP_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_FX, "Rate.Creature.Elite.RAREELITE.HP_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_FX, "Rate.Creature.Elite.WORLDBOSS.HP_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_FX, "Rate.Creature.Elite.RARE.HP_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_MC, "Rate.Creature.Normal.HP_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_MC, "Rate.Creature.Elite.Elite.HP_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_MC, "Rate.Creature.Elite.RAREELITE.HP_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_MC, "Rate.Creature.Elite.WORLDBOSS.HP_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_MC, "Rate.Creature.Elite.RARE.HP_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_HL, "Rate.Creature.Normal.HP_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_HL, "Rate.Creature.Elite.Elite.HP_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_HL, "Rate.Creature.Elite.RAREELITE.HP_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_HL, "Rate.Creature.Elite.WORLDBOSS.HP_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_HL, "Rate.Creature.Elite.RARE.HP_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_BWL, "Rate.Creature.Normal.HP_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_BWL, "Rate.Creature.Elite.Elite.HP_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_BWL, "Rate.Creature.Elite.RAREELITE.HP_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_BWL, "Rate.Creature.Elite.WORLDBOSS.HP_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_BWL, "Rate.Creature.Elite.RARE.HP_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_TAQ, "Rate.Creature.Normal.HP_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_TAQ, "Rate.Creature.Elite.Elite.HP_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_TAQ, "Rate.Creature.Elite.RAREELITE.HP_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_TAQ, "Rate.Creature.Elite.WORLDBOSS.HP_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_TAQ, "Rate.Creature.Elite.RARE.HP_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_NAXX, "Rate.Creature.Normal.HP_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_NAXX, "Rate.Creature.Elite.Elite.HP_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_NAXX, "Rate.Creature.Elite.RAREELITE.HP_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_NAXX, "Rate.Creature.Elite.WORLDBOSS.HP_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_NAXX, "Rate.Creature.Elite.RARE.HP_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_HP_AS, "Rate.Creature.Normal.HP_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_HP_AS, "Rate.Creature.Elite.Elite.HP_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_HP_AS, "Rate.Creature.Elite.RAREELITE.HP_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_HP_AS, "Rate.Creature.Elite.WORLDBOSS.HP_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_HP_AS, "Rate.Creature.Elite.RARE.HP_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE, "Rate.Creature.Normal.SpellDamage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE, "Rate.Creature.Elite.Elite.SpellDamage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE, "Rate.Creature.Elite.RAREELITE.SpellDamage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE, "Rate.Creature.Elite.WORLDBOSS.SpellDamage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE, "Rate.Creature.Elite.RARE.SpellDamage", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_ZUG, "Rate.Creature.Normal.SPELLDAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_ZUG, "Rate.Creature.Elite.Elite.SPELLDAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_ZUG, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_ZUG, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_ZUG, "Rate.Creature.Elite.RARE.SPELLDAMAGE_ZUG", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_FX, "Rate.Creature.Normal.SPELLDAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_FX, "Rate.Creature.Elite.Elite.SPELLDAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_FX, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_FX, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_FX, "Rate.Creature.Elite.RARE.SPELLDAMAGE_FX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_MC, "Rate.Creature.Normal.SPELLDAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_MC, "Rate.Creature.Elite.Elite.SPELLDAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_MC, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_MC, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_MC, "Rate.Creature.Elite.RARE.SPELLDAMAGE_MC", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_HL, "Rate.Creature.Normal.SPELLDAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_HL, "Rate.Creature.Elite.Elite.SPELLDAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_HL, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_HL, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_HL, "Rate.Creature.Elite.RARE.SPELLDAMAGE_HL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_BWL, "Rate.Creature.Normal.SPELLDAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_BWL, "Rate.Creature.Elite.Elite.SPELLDAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_BWL, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_BWL, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_BWL, "Rate.Creature.Elite.RARE.SPELLDAMAGE_BWL", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_TAQ, "Rate.Creature.Normal.SPELLDAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_TAQ, "Rate.Creature.Elite.Elite.SPELLDAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_TAQ, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_TAQ, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_TAQ, "Rate.Creature.Elite.RARE.SPELLDAMAGE_TAQ", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_NAXX, "Rate.Creature.Normal.SPELLDAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_NAXX, "Rate.Creature.Elite.Elite.SPELLDAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_NAXX, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_NAXX, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_NAXX, "Rate.Creature.Elite.RARE.SPELLDAMAGE_NAXX", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_NORMAL_SPELLDAMAGE_AS, "Rate.Creature.Normal.SPELLDAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_ELITE_SPELLDAMAGE_AS, "Rate.Creature.Elite.Elite.SPELLDAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RAREELITE_SPELLDAMAGE_AS, "Rate.Creature.Elite.RAREELITE.SPELLDAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_WORLDBOSS_SPELLDAMAGE_AS, "Rate.Creature.Elite.WORLDBOSS.SPELLDAMAGE_AS", 1.0f);
	setConfigPos(CONFIG_FLOAT_RATE_CREATURE_ELITE_RARE_SPELLDAMAGE_AS, "Rate.Creature.Elite.RARE.SPELLDAMAGE_AS", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_CREATURE_AGGRO, "Rate.Creature.Aggro", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_REST_INGAME,                    "Rate.Rest.InGame", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_REST_OFFLINE_IN_TAVERN_OR_CITY, "Rate.Rest.Offline.InTavernOrCity", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_REST_OFFLINE_IN_WILDERNESS,     "Rate.Rest.Offline.InWilderness", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_DAMAGE_FALL,  "Rate.Damage.Fall", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_AUCTION_TIME, "Rate.Auction.Time", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_AUCTION_DEPOSIT, "Rate.Auction.Deposit", 1.0f);
    setConfig(CONFIG_FLOAT_RATE_AUCTION_CUT,     "Rate.Auction.Cut", 1.0f);
    setConfig(CONFIG_UINT32_AUCTION_DEPOSIT_MIN, "Auction.Deposit.Min", 0);
    setConfig(CONFIG_FLOAT_RATE_HONOR, "Rate.Honor", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_MINING_AMOUNT, "Rate.Mining.Amount", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_MINING_NEXT,   "Rate.Mining.Next", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_INSTANCE_RESET_TIME, "Rate.InstanceResetTime", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_TALENT, "Rate.Talent", 1.0f);
    setConfigPos(CONFIG_FLOAT_RATE_CORPSE_DECAY_LOOTED, "Rate.Corpse.Decay.Looted", 0.0f);

    setConfigMinMax(CONFIG_FLOAT_RATE_TARGET_POS_RECALCULATION_RANGE, "TargetPosRecalculateRange", 1.5f, CONTACT_DISTANCE, ATTACK_DISTANCE);

    setConfigPos(CONFIG_FLOAT_RATE_DURABILITY_LOSS_DAMAGE, "DurabilityLossChance.Damage", 0.5f);
    setConfigPos(CONFIG_FLOAT_RATE_DURABILITY_LOSS_ABSORB, "DurabilityLossChance.Absorb", 0.5f);
    setConfigPos(CONFIG_FLOAT_RATE_DURABILITY_LOSS_PARRY,  "DurabilityLossChance.Parry",  0.05f);
    setConfigPos(CONFIG_FLOAT_RATE_DURABILITY_LOSS_BLOCK,  "DurabilityLossChance.Block",  0.05f);

    setConfigPos(CONFIG_FLOAT_LISTEN_RANGE_SAY,       "ListenRange.Say",       40.0f);
    setConfigPos(CONFIG_FLOAT_LISTEN_RANGE_YELL,      "ListenRange.Yell",     300.0f);
    setConfigPos(CONFIG_FLOAT_LISTEN_RANGE_TEXTEMOTE, "ListenRange.TextEmote", 40.0f);

    setConfigPos(CONFIG_FLOAT_GROUP_XP_DISTANCE, "MaxGroupXPDistance", 74.0f);
    setConfigPos(CONFIG_FLOAT_SIGHT_GUARDER,     "GuarderSight",       50.0f);
    setConfigPos(CONFIG_FLOAT_SIGHT_MONSTER,     "MonsterSight",       50.0f);

    setConfigPos(CONFIG_FLOAT_CREATURE_FAMILY_ASSISTANCE_RADIUS,      "CreatureFamilyAssistanceRadius",     10.0f);
    setConfigPos(CONFIG_FLOAT_CREATURE_FAMILY_FLEE_ASSISTANCE_RADIUS, "CreatureFamilyFleeAssistanceRadius", 30.0f);

	setConfigPos(CONFIG_FLOAT_XIOULIFEI, "Rate.XiouLiFei", 1.0f);
    ///- Read other configuration items from the config file
	// movement anticheat
    m_MvAnticheatEnable                     = sConfig.GetBoolDefault("Anticheat.Movement.Enable",false);
    m_MvAnticheatKick                       = sConfig.GetBoolDefault("Anticheat.Movement.Kick",false);
    m_MvAnticheatAlarmCount                 = (uint32)sConfig.GetIntDefault("Anticheat.Movement.AlarmCount", 5);
    m_MvAnticheatAlarmPeriod                = (uint32)sConfig.GetIntDefault("Anticheat.Movement.AlarmTime", 5000);
    m_MvAntiCheatBan                        = (unsigned char)sConfig.GetIntDefault("Anticheat.Movement.BanType",0);
    m_MvAnticheatBanTime                    = (uint32)sConfig.GetIntDefault("Anticheat.Movement.BanTime",60);
    m_MvAnticheatGmLevel                    = (unsigned char)sConfig.GetIntDefault("Anticheat.Movement.GmLevel",0);
    m_MvAnticheatKill                       = sConfig.GetBoolDefault("Anticheat.Movement.Kill",false);
    m_MvAnticheatMaxXYT                     = sConfig.GetFloatDefault("Anticheat.Movement.MaxXYT",0.008f);
	m_MvAnticheatMaxXGJZQ                     = sConfig.GetFloatDefault("Anticheat.Movement.MaxXGJZQ",0.015f);
	m_MvAnticheatMaxXPTGH                     = sConfig.GetFloatDefault("Anticheat.Movement.MaxXPTGH",0.01f);
	m_MvAnticheatMaxXPTZQ                     = sConfig.GetFloatDefault("Anticheat.Movement.MaxXPTZQ",0.012f);
	m_MvAnticheatMaxXGJGH                     = sConfig.GetFloatDefault("Anticheat.Movement.MaxXGJGH",0.013f);
	m_MvAnticheatMaxXGJYY                     = sConfig.GetFloatDefault("Anticheat.Movement.MaxXGJYY",0.012f);
	m_MvAnticheatMaxXDBX                      = sConfig.GetFloatDefault("Anticheat.Movement.MaxXDBX", 0.012f);
    m_MvAnticheatIgnoreAfterTeleport        = (uint16)sConfig.GetIntDefault("Anticheat.Movement.IgnoreSecAfterTeleport",10);
    setConfigMinMax(CONFIG_UINT32_COMPRESSION, "Compression", 1, 1, 9);
    setConfig(CONFIG_BOOL_ADDON_CHANNEL, "AddonChannel", true);
    setConfig(CONFIG_BOOL_CLEAN_CHARACTER_DB, "CleanCharacterDB", true);
    setConfig(CONFIG_BOOL_GRID_UNLOAD, "GridUnload", true);

    std::string forceLoadGridOnMaps = sConfig.GetStringDefault("LoadAllGridsOnMaps", "");
    if (!forceLoadGridOnMaps.empty())
    {
        m_configForceLoadMapIds = new std::set<uint32>;
        unsigned int pos = 0;
        unsigned int id;
        VMAP::VMapFactory::chompAndTrim(forceLoadGridOnMaps);
        while (VMAP::VMapFactory::getNextId(forceLoadGridOnMaps, pos, id))
            m_configForceLoadMapIds->insert(id);
    }

    setConfig(CONFIG_UINT32_INTERVAL_SAVE, "PlayerSave.Interval", 15 * MINUTE * IN_MILLISECONDS);
    setConfigMinMax(CONFIG_UINT32_MIN_LEVEL_STAT_SAVE, "PlayerSave.Stats.MinLevel", 0, 0, MAX_LEVEL);
    setConfig(CONFIG_BOOL_STATS_SAVE_ONLY_ON_LOGOUT, "PlayerSave.Stats.SaveOnlyOnLogout", true);

    setConfigMin(CONFIG_UINT32_INTERVAL_GRIDCLEAN, "GridCleanUpDelay", 5 * MINUTE * IN_MILLISECONDS, MIN_GRID_DELAY);
    if (reload)
        sMapMgr.SetGridCleanUpDelay(getConfig(CONFIG_UINT32_INTERVAL_GRIDCLEAN));

	setConfig(CONFIG_UINT32_NUMTHREADS, "MapUpdate.Threads", 3);
    setConfig(CONFIG_BOOL_THREADS_DYNAMIC,"MapUpdate.DynamicThreadsCount", false); 
 
    setConfigMinMax(CONFIG_FLOAT_LOADBALANCE_HIGHVALUE, "MapUpdate.LoadBalanceHighValue", 0.8f, 0.5f, 1.0f); 
    setConfigMinMax(CONFIG_FLOAT_LOADBALANCE_LOWVALUE, "MapUpdate.LoadBalanceLowValue", 0.2f, 0.0f, 0.5f);

    setConfigMin(CONFIG_UINT32_INTERVAL_MAPUPDATE, "MapUpdateInterval", 100, MIN_MAP_UPDATE_DELAY);
    if (reload)
        sMapMgr.SetMapUpdateInterval(getConfig(CONFIG_UINT32_INTERVAL_MAPUPDATE));

    setConfig(CONFIG_UINT32_INTERVAL_CHANGEWEATHER, "ChangeWeatherInterval", 10 * MINUTE * IN_MILLISECONDS);

    if (configNoReload(reload, CONFIG_UINT32_PORT_WORLD, "WorldServerPort", DEFAULT_WORLDSERVER_PORT))
        setConfig(CONFIG_UINT32_PORT_WORLD, "WorldServerPort", DEFAULT_WORLDSERVER_PORT);

    if (configNoReload(reload, CONFIG_UINT32_GAME_TYPE, "GameType", 0))
        setConfig(CONFIG_UINT32_GAME_TYPE, "GameType", 0);

    if (configNoReload(reload, CONFIG_UINT32_REALM_ZONE, "RealmZone", REALM_ZONE_DEVELOPMENT))
        setConfig(CONFIG_UINT32_REALM_ZONE, "RealmZone", REALM_ZONE_DEVELOPMENT);

    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_ACCOUNTS,            "AllowTwoSide.Accounts", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_CHAT,    "AllowTwoSide.Interaction.Chat", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_CHANNEL, "AllowTwoSide.Interaction.Channel", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_GROUP,   "AllowTwoSide.Interaction.Group", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_GUILD,   "AllowTwoSide.Interaction.Guild", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_TRADE,   "AllowTwoSide.Interaction.Trade", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_AUCTION, "AllowTwoSide.Interaction.Auction", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_INTERACTION_MAIL,    "AllowTwoSide.Interaction.Mail", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_WHO_LIST,            "AllowTwoSide.WhoList", false);
    setConfig(CONFIG_BOOL_ALLOW_TWO_SIDE_ADD_FRIEND,          "AllowTwoSide.AddFriend", false);

    setConfig(CONFIG_UINT32_STRICT_PLAYER_NAMES,  "StrictPlayerNames",  0);
    setConfig(CONFIG_UINT32_STRICT_CHARTER_NAMES, "StrictCharterNames", 0);
    setConfig(CONFIG_UINT32_STRICT_PET_NAMES,     "StrictPetNames",     0);

    setConfigMinMax(CONFIG_UINT32_MIN_PLAYER_NAME,  "MinPlayerName",  2, 1, MAX_PLAYER_NAME);
    setConfigMinMax(CONFIG_UINT32_MIN_CHARTER_NAME, "MinCharterName", 2, 1, MAX_CHARTER_NAME);
    setConfigMinMax(CONFIG_UINT32_MIN_PET_NAME,     "MinPetName",     2, 1, MAX_PET_NAME);

    setConfig(CONFIG_UINT32_CHARACTERS_CREATING_DISABLED, "CharactersCreatingDisabled", 0);

    setConfigMinMax(CONFIG_UINT32_CHARACTERS_PER_REALM, "CharactersPerRealm", 10, 1, 10);

    // must be after CONFIG_UINT32_CHARACTERS_PER_REALM
    setConfigMin(CONFIG_UINT32_CHARACTERS_PER_ACCOUNT, "CharactersPerAccount", 50, getConfig(CONFIG_UINT32_CHARACTERS_PER_REALM));

    setConfigMinMax(CONFIG_UINT32_SKIP_CINEMATICS, "SkipCinematics", 0, 0, 2);

    if (configNoReload(reload, CONFIG_UINT32_MAX_PLAYER_LEVEL, "MaxPlayerLevel", DEFAULT_MAX_LEVEL))
        setConfigMinMax(CONFIG_UINT32_MAX_PLAYER_LEVEL, "MaxPlayerLevel", DEFAULT_MAX_LEVEL, 1, DEFAULT_MAX_LEVEL);

    setConfigMinMax(CONFIG_UINT32_START_PLAYER_LEVEL, "StartPlayerLevel", 1, 1, getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL));

    setConfigMinMax(CONFIG_UINT32_START_PLAYER_MONEY, "StartPlayerMoney", 0, 0, MAX_MONEY_AMOUNT);

    setConfig(CONFIG_UINT32_MAX_HONOR_POINTS, "MaxHonorPoints", 75000);

    setConfigMinMax(CONFIG_UINT32_START_HONOR_POINTS, "StartHonorPoints", 0, 0, getConfig(CONFIG_UINT32_MAX_HONOR_POINTS));

    setConfigMin(CONFIG_UINT32_MIN_HONOR_KILLS, "MinHonorKills", HONOR_STANDING_MIN_KILL, 1);

    setConfigMinMax(CONFIG_UINT32_MAINTENANCE_DAY, "MaintenanceDay", 4, 0, 6);
	setConfig(CONFIG_UINT32_MAINTENANCE_DAY_XP1, "MaintenanceDayXp6", 6);
	setConfig(CONFIG_UINT32_MAINTENANCE_DAY_XP2, "MaintenanceDayXp7", 7);

    setConfig(CONFIG_BOOL_ALL_TAXI_PATHS, "AllFlightPaths", false);
	setConfig(CONFIG_BOOL_ALL_EXPLORED, "AllMapsExplored", false);

    setConfig(CONFIG_BOOL_INSTANCE_IGNORE_LEVEL, "Instance.IgnoreLevel", false);
    setConfig(CONFIG_BOOL_INSTANCE_IGNORE_RAID,  "Instance.IgnoreRaid", false);

    setConfig(CONFIG_BOOL_CAST_UNSTUCK, "CastUnstuck", true);
    setConfig(CONFIG_UINT32_MAX_SPELL_CASTS_IN_CHAIN, "MaxSpellCastsInChain", 20);
    setConfig(CONFIG_UINT32_RABBIT_DAY, "RabbitDay", 0);

    setConfig(CONFIG_UINT32_INSTANCE_RESET_TIME_HOUR, "Instance.ResetTimeHour", 4);
    setConfig(CONFIG_UINT32_INSTANCE_UNLOAD_DELAY,    "Instance.UnloadDelay", 30 * MINUTE * IN_MILLISECONDS);

    setConfigMinMax(CONFIG_UINT32_MAX_PRIMARY_TRADE_SKILL, "MaxPrimaryTradeSkill", 2, 0, 10);

    setConfigMinMax(CONFIG_UINT32_TRADE_SKILL_GMIGNORE_MAX_PRIMARY_COUNT, "TradeSkill.GMIgnore.MaxPrimarySkillsCount", SEC_CONSOLE, SEC_PLAYER, SEC_CONSOLE);
    setConfigMinMax(CONFIG_UINT32_TRADE_SKILL_GMIGNORE_LEVEL, "TradeSkill.GMIgnore.Level", SEC_CONSOLE, SEC_PLAYER, SEC_CONSOLE);
    setConfigMinMax(CONFIG_UINT32_TRADE_SKILL_GMIGNORE_SKILL, "TradeSkill.GMIgnore.Skill", SEC_CONSOLE, SEC_PLAYER, SEC_CONSOLE);

    setConfigMinMax(CONFIG_UINT32_MIN_PETITION_SIGNS, "MinPetitionSigns", 9, 0, 9);

    setConfig(CONFIG_UINT32_GM_LOGIN_STATE,    "GM.LoginState",    2);
    setConfig(CONFIG_UINT32_GM_VISIBLE_STATE,  "GM.Visible",       2);
    setConfig(CONFIG_UINT32_GM_ACCEPT_TICKETS, "GM.AcceptTickets", 2);
    setConfig(CONFIG_UINT32_GM_CHAT,           "GM.Chat",          2);
    setConfig(CONFIG_UINT32_GM_WISPERING_TO,   "GM.WhisperingTo",  2);

    setConfig(CONFIG_UINT32_GM_LEVEL_IN_GM_LIST,  "GM.InGMList.Level",  SEC_ADMINISTRATOR);
    setConfig(CONFIG_UINT32_GM_LEVEL_IN_WHO_LIST, "GM.InWhoList.Level", SEC_ADMINISTRATOR);
    setConfig(CONFIG_BOOL_GM_LOG_TRADE,           "GM.LogTrade", false);

    setConfigMinMax(CONFIG_UINT32_START_GM_LEVEL, "GM.StartLevel", 1, getConfig(CONFIG_UINT32_START_PLAYER_LEVEL), MAX_LEVEL);
    setConfig(CONFIG_BOOL_GM_LOWER_SECURITY, "GM.LowerSecurity", false);
    setConfig(CONFIG_UINT32_GM_INVISIBLE_AURA, "GM.InvisibleAura", 31748);

    setConfig(CONFIG_UINT32_GROUP_VISIBILITY, "Visibility.GroupMode", 0);

    setConfig(CONFIG_UINT32_MAIL_DELIVERY_DELAY, "MailDeliveryDelay", HOUR);

    setConfigMin(CONFIG_UINT32_MASS_MAILER_SEND_PER_TICK, "MassMailer.SendPerTick", 10, 1);

    setConfig(CONFIG_UINT32_UPTIME_UPDATE, "UpdateUptimeInterval", 10);
    if (reload)
    {
        m_timers[WUPDATE_UPTIME].SetInterval(getConfig(CONFIG_UINT32_UPTIME_UPDATE)*MINUTE * IN_MILLISECONDS);
        m_timers[WUPDATE_UPTIME].Reset();
    }

    setConfig(CONFIG_UINT32_SKILL_CHANCE_ORANGE, "SkillChance.Orange", 100);
    setConfig(CONFIG_UINT32_SKILL_CHANCE_YELLOW, "SkillChance.Yellow", 75);
    setConfig(CONFIG_UINT32_SKILL_CHANCE_GREEN,  "SkillChance.Green",  25);
    setConfig(CONFIG_UINT32_SKILL_CHANCE_GREY,   "SkillChance.Grey",   0);

    setConfig(CONFIG_UINT32_SKILL_CHANCE_MINING_STEPS,   "SkillChance.MiningSteps",   75);
    setConfig(CONFIG_UINT32_SKILL_CHANCE_SKINNING_STEPS, "SkillChance.SkinningSteps", 75);

    setConfig(CONFIG_UINT32_SKILL_GAIN_CRAFTING,  "SkillGain.Crafting",  1);
    setConfig(CONFIG_UINT32_SKILL_GAIN_DEFENSE,   "SkillGain.Defense",   1);
    setConfig(CONFIG_UINT32_SKILL_GAIN_GATHERING, "SkillGain.Gathering", 1);
    setConfig(CONFIG_UINT32_SKILL_GAIN_WEAPON,       "SkillGain.Weapon",    1);

    setConfig(CONFIG_BOOL_SKILL_FAIL_LOOT_FISHING,         "SkillFail.Loot.Fishing", false);
    setConfig(CONFIG_BOOL_SKILL_FAIL_GAIN_FISHING,         "SkillFail.Gain.Fishing", false);
    setConfig(CONFIG_BOOL_SKILL_FAIL_POSSIBLE_FISHINGPOOL, "SkillFail.Possible.FishingPool", true);

    setConfig(CONFIG_UINT32_MAX_OVERSPEED_PINGS, "MaxOverspeedPings", 2);
    if (getConfig(CONFIG_UINT32_MAX_OVERSPEED_PINGS) != 0 && getConfig(CONFIG_UINT32_MAX_OVERSPEED_PINGS) < 2)
    {
        sLog.outError("MaxOverspeedPings (%i) must be in range 2..infinity (or 0 to disable check). Set to 2.", getConfig(CONFIG_UINT32_MAX_OVERSPEED_PINGS));
        setConfig(CONFIG_UINT32_MAX_OVERSPEED_PINGS, 2);
    }

    setConfig(CONFIG_BOOL_SAVE_RESPAWN_TIME_IMMEDIATELY, "SaveRespawnTimeImmediately", true);
    setConfig(CONFIG_BOOL_WEATHER, "ActivateWeather", true);

    setConfig(CONFIG_BOOL_ALWAYS_MAX_SKILL_FOR_LEVEL, "AlwaysMaxSkillForLevel", false);

    setConfig(CONFIG_UINT32_CHATFLOOD_MESSAGE_COUNT, "ChatFlood.MessageCount", 10);
    setConfig(CONFIG_UINT32_CHATFLOOD_MESSAGE_DELAY, "ChatFlood.MessageDelay", 1);
    setConfig(CONFIG_UINT32_CHATFLOOD_MUTE_TIME,     "ChatFlood.MuteTime", 10);

	setConfig(CONFIG_UINT32_CHATFLOOD_MESSAGE_COUNT_A, "ChatFlood.PlayerMessageCount", 10);
	setConfig(CONFIG_UINT32_CHATFLOOD_MESSAGE_DELAY_A, "ChatFlood.PlayerMessageDelay", 1);
	setConfig(CONFIG_UINT32_CHATFLOOD_MUTE_TIME_A, "ChatFlood.PlayerMuteTime", 10);

    setConfig(CONFIG_BOOL_EVENT_ANNOUNCE, "Event.Announce", false);

    setConfig(CONFIG_UINT32_CREATURE_FAMILY_ASSISTANCE_DELAY, "CreatureFamilyAssistanceDelay", 1500);
    setConfig(CONFIG_UINT32_CREATURE_FAMILY_FLEE_DELAY,       "CreatureFamilyFleeDelay",       7000);

    setConfig(CONFIG_UINT32_WORLD_BOSS_LEVEL_DIFF, "WorldBossLevelDiff", 3);

    setConfigMinMax(CONFIG_INT32_QUEST_LOW_LEVEL_HIDE_DIFF, "Quests.LowLevelHideDiff", 4, -1, MAX_LEVEL);
    setConfigMinMax(CONFIG_INT32_QUEST_HIGH_LEVEL_HIDE_DIFF, "Quests.HighLevelHideDiff", 7, -1, MAX_LEVEL);

    setConfig(CONFIG_BOOL_QUEST_IGNORE_RAID, "Quests.IgnoreRaid", false);

    setConfig(CONFIG_BOOL_DETECT_POS_COLLISION, "DetectPosCollision", true);

    setConfig(CONFIG_BOOL_RESTRICTED_LFG_CHANNEL,      "Channel.RestrictedLfg", true);
	setConfig(CONFIG_BOOL_SILENTLY_GM_JOIN_TO_CHANNEL, "Channel.SilentlyGMJoin", true);

    setConfig(CONFIG_BOOL_CHAT_FAKE_MESSAGE_PREVENTING, "ChatFakeMessagePreventing", false);

    setConfig(CONFIG_UINT32_CHAT_STRICT_LINK_CHECKING_SEVERITY, "ChatStrictLinkChecking.Severity", 0);
    setConfig(CONFIG_UINT32_CHAT_STRICT_LINK_CHECKING_KICK,     "ChatStrictLinkChecking.Kick", 0);

    setConfig(CONFIG_BOOL_CORPSE_EMPTY_LOOT_SHOW,      "Corpse.EmptyLootShow", true);
    setConfig(CONFIG_UINT32_CORPSE_DECAY_NORMAL,    "Corpse.Decay.NORMAL",    300);
    setConfig(CONFIG_UINT32_CORPSE_DECAY_RARE,      "Corpse.Decay.RARE",      900);
    setConfig(CONFIG_UINT32_CORPSE_DECAY_ELITE,     "Corpse.Decay.ELITE",     600);
    setConfig(CONFIG_UINT32_CORPSE_DECAY_RAREELITE, "Corpse.Decay.RAREELITE", 1200);
    setConfig(CONFIG_UINT32_CORPSE_DECAY_WORLDBOSS, "Corpse.Decay.WORLDBOSS", 3600);

    setConfig(CONFIG_INT32_DEATH_SICKNESS_LEVEL, "Death.SicknessLevel", 11);

    setConfig(CONFIG_BOOL_DEATH_CORPSE_RECLAIM_DELAY_PVP, "Death.CorpseReclaimDelay.PvP", true);
    setConfig(CONFIG_BOOL_DEATH_CORPSE_RECLAIM_DELAY_PVE, "Death.CorpseReclaimDelay.PvE", true);
    setConfig(CONFIG_BOOL_DEATH_BONES_WORLD,              "Death.Bones.World", true);
    setConfig(CONFIG_BOOL_DEATH_BONES_BG,                 "Death.Bones.Battleground", true);
    setConfigMinMax(CONFIG_FLOAT_GHOST_RUN_SPEED_WORLD,   "Death.Ghost.RunSpeed.World", 1.0f, 0.1f, 10.0f);
    setConfigMinMax(CONFIG_FLOAT_GHOST_RUN_SPEED_BG,      "Death.Ghost.RunSpeed.Battleground", 1.0f, 0.1f, 10.0f);

    setConfig(CONFIG_FLOAT_THREAT_RADIUS, "ThreatRadius", 100.0f);
    setConfigMin(CONFIG_UINT32_CREATURE_RESPAWN_AGGRO_DELAY, "CreatureRespawnAggroDelay", 5000, 0);

    setConfig(CONFIG_BOOL_BATTLEGROUND_CAST_DESERTER,                  "Battleground.CastDeserter", true);
    setConfigMinMax(CONFIG_UINT32_BATTLEGROUND_QUEUE_ANNOUNCER_JOIN,   "Battleground.QueueAnnouncer.Join", 0, 0, 2);
    setConfig(CONFIG_BOOL_BATTLEGROUND_QUEUE_ANNOUNCER_START,          "Battleground.QueueAnnouncer.Start", false);
    setConfig(CONFIG_BOOL_BATTLEGROUND_SCORE_STATISTICS,               "Battleground.ScoreStatistics", false);
    setConfig(CONFIG_UINT32_BATTLEGROUND_INVITATION_TYPE,              "Battleground.InvitationType", 0);
    setConfig(CONFIG_UINT32_BATTLEGROUND_PREMATURE_FINISH_TIMER,       "BattleGround.PrematureFinishTimer", 5 * MINUTE * IN_MILLISECONDS);
    setConfig(CONFIG_UINT32_BATTLEGROUND_PREMADE_GROUP_WAIT_FOR_MATCH, "BattleGround.PremadeGroupWaitForMatch", 0);
    setConfig(CONFIG_BOOL_OUTDOORPVP_SI_ENABLED,                       "OutdoorPvp.SIEnabled", true);
    setConfig(CONFIG_BOOL_OUTDOORPVP_EP_ENABLED,                       "OutdoorPvp.EPEnabled", true);

	setConfig(CONFIG_BOOL_BATTLEGROUND_JIANCE, "Battleground.Jiance", false);
	setConfig(CONFIG_UINT32_BATTLEGROUND_IP_PLAYERCONT, "Battleground.IP_PlayerCont", 1);
	setConfig(CONFIG_UINT32_BATTLEGROUND_PLAYERCONT_WINNER, "Battleground.Player_Winner", 5);
	setConfig(CONFIG_UINT32_BATTLEGROUND_PLAYERCONT_LOSER, "Battleground.Player_Loser", 5);
	setConfig(CONFIG_UINT32_BATTLEGROUND_TIME,    "Battleground.Time", 120000);
	setConfig(CONFIG_UINT32_BATTLEGROUND_PLAYER_MINDAMAGE, "Battleground.MinDamage", 10000);
	setConfig(CONFIG_UINT32_BATTLEGROUND_PLAYER_MINHEALINGDONE, "Battleground.MinHealingDone", 10000);
	setConfig(CONFIG_UINT32_PLAYER_INSTANCES_PER_HOUR, "AccountInstancesPerHour", 5);
	setConfig(CONFIG_BOOL_PLAYER_INSTANCES_PER_HOUR_DAKAI, "AccountInstancesPerHour.DaKai", false);

	setConfig(CONFIG_UINT32_CREATURE_ELITE_NORMAL, "Rate.Creature.Normal.Time", 10);
	setConfig(CREATURE_ELITE_ELITE_ELITE, "Rate.Creature.Elite.Time", 10);
	setConfig(CREATURE_ELITE_RAREELITE_RAREELITE, "Rate.Creature.Rareelite.Time", 10);
	setConfig(CREATURE_ELITE_WORLDBOSS_WORLDBOSS, "Rate.Creature.WorldBoss.Time", 10);
	setConfig(CREATURE_ELITE_RARE_RARE, "Rate.Creature.Rare.Time", 10);
	//Custom
	setConfig(CONFIG_BOOL_GUILD, "Battleground.Guild.On", false);
	setConfig(CONFIG_BOOL_ONYXIA, "Onyxia'sLair.On", false);

	setConfig(CONFIG_BOOL_OUTDOORPVP_TS_ENABLED,                       "OutdoorPvP.TSEnabled", true);
	setConfig(CONFIG_BOOL_OUTDOORPVP_ZG_ENABLED,                       "OutdoorPvP.ZUGEnabled", true);
	setConfig(CONFIG_BOOL_OUTDOORPVP_FX_ENABLED,                       "OutdoorPvP.FXEnabled", true);
	setConfig(CONFIG_BOOL_OUTDOORPVP_EY_ENABLED,                       "OutdoorPvP.EYEnabled", true);
	setConfig(CONFIG_BOOL_AOSHAN, "Command.aoshan", false);
	setConfig(CONFIG_BOOL_ZHANGE, "Command.ZhanGe", false);
	setConfig(CONFIG_BOOL_ALX, "Command.ALX", false);
	setConfig(CONFIG_BOOL_FEIJI_SHUNFEI,      "FeiJi.ShunFei", false);

	setConfig(CONFIG_BOOL_TAQ_KAIENRENWU, "Taq.KaiMenRenWu", false);

	setConfig(CONFIG_UINT32_HONORAD_TIME_START_1, "Honor1.TimeStart", 27000);
	setConfig(CONFIG_UINT32_HONORAD_TIME_END_1, "Honor1.TimeEnd", 37800);

	setConfig(CONFIG_UINT32_HONORAD_TIME_START_2, "Honor2.TimeStart", 41400);
	setConfig(CONFIG_UINT32_HONORAD_TIME_END_2, "Honor2.TimeEnd", 64800);

	setConfig(CONFIG_UINT32_HONORAD_TIME_START1_1, "Honor1.TimeStart_ZhouMo", 27000);
	setConfig(CONFIG_UINT32_HONORAD_TIME_END1_1, "Honor1.TimeEnd_ZhouMo", 37800);

	setConfig(CONFIG_UINT32_HONORAD_TIME_START2_2, "Honor2.TimeStart_ZhouMo", 37800);
	setConfig(CONFIG_UINT32_HONORAD_TIME_END2_2, "Honor2.TimeEnd_ZhouMo", 64800);

	setConfig(CONFIG_UINT32_GROUPLEADER_RECONNECT_PERIOD, "GroupLeaderReconnectPeriod", 120);

	setConfigPos(CONFIG_FLOAT_HONOR_PLAYER_MAX, "PlayerHonorMax", 35000.0f);

    setConfig(CONFIG_BOOL_KICK_PLAYER_ON_BAD_PACKET, "Network.KickOnBadPacket", false);

    setConfig(CONFIG_BOOL_PLAYER_COMMANDS, "PlayerCommands", true);

	setConfig(CONFIG_BOOL_SAFE_LOCK, "Safe.Lock", true);

    setConfig(CONFIG_UINT32_INSTANT_LOGOUT, "InstantLogout", SEC_MODERATOR);

    setConfigMin(CONFIG_UINT32_GUILD_EVENT_LOG_COUNT, "Guild.EventLogRecordsCount", GUILD_EVENTLOG_MAX_RECORDS, GUILD_EVENTLOG_MAX_RECORDS);

    setConfig(CONFIG_UINT32_TIMERBAR_FATIGUE_GMLEVEL, "TimerBar.Fatigue.GMLevel", SEC_CONSOLE);
    setConfig(CONFIG_UINT32_TIMERBAR_FATIGUE_MAX,     "TimerBar.Fatigue.Max", 60);
    setConfig(CONFIG_UINT32_TIMERBAR_BREATH_GMLEVEL,  "TimerBar.Breath.GMLevel", SEC_CONSOLE);
    setConfig(CONFIG_UINT32_TIMERBAR_BREATH_MAX,      "TimerBar.Breath.Max", 180);
    setConfig(CONFIG_UINT32_TIMERBAR_FIRE_GMLEVEL,    "TimerBar.Fire.GMLevel", SEC_CONSOLE);
    setConfig(CONFIG_UINT32_TIMERBAR_FIRE_MAX,        "TimerBar.Fire.Max", 1);
	setConfig(CONFIG_BOOL_WORLD_PVP_ON, "World.PVP.On", false);
	setConfig(CONFIG_BOOL_WORLD_CHAT_ON, "World.Chat.On", false);
	setConfig(CONFIG_UINT32_JF_OR_MONEY, "Jf.Or.Money", 0);
	setConfig(CONFIG_UINT32_CHAT_COUNT, "Chat.Count", 0);

    setConfig(CONFIG_BOOL_PET_UNSUMMON_AT_MOUNT,      "PetUnsummonAtMount", false);

	setConfig(CONFIG_BOOL_BATTLEGROUND_MAIL, "BattleGround.Mail", true);

    m_relocation_ai_notify_delay = sConfig.GetIntDefault("Visibility.AIRelocationNotifyDelay", 1000u);
    m_relocation_lower_limit_sq  = pow(sConfig.GetFloatDefault("Visibility.RelocationLowerLimit", 10), 2);

    m_VisibleUnitGreyDistance = sConfig.GetFloatDefault("Visibility.Distance.Grey.Unit", 1);
    if (m_VisibleUnitGreyDistance >  MAX_VISIBILITY_DISTANCE)
    {
        sLog.outError("Visibility.Distance.Grey.Unit can't be greater %f", MAX_VISIBILITY_DISTANCE);
        m_VisibleUnitGreyDistance = MAX_VISIBILITY_DISTANCE;
    }
    m_VisibleObjectGreyDistance = sConfig.GetFloatDefault("Visibility.Distance.Grey.Object", 10);
    if (m_VisibleObjectGreyDistance >  MAX_VISIBILITY_DISTANCE)
    {
        sLog.outError("Visibility.Distance.Grey.Object can't be greater %f", MAX_VISIBILITY_DISTANCE);
        m_VisibleObjectGreyDistance = MAX_VISIBILITY_DISTANCE;
    }

    // visibility on continents
    m_MaxVisibleDistanceOnContinents      = sConfig.GetFloatDefault("Visibility.Distance.Continents",     DEFAULT_VISIBILITY_DISTANCE);
    if (m_MaxVisibleDistanceOnContinents < 45 * getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO))
    {
        sLog.outError("Visibility.Distance.Continents can't be less max aggro radius %f", 45 * getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO));
        m_MaxVisibleDistanceOnContinents = 45 * getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO);
    }
    else if (m_MaxVisibleDistanceOnContinents + m_VisibleUnitGreyDistance >  MAX_VISIBILITY_DISTANCE)
    {
        sLog.outError("Visibility.Distance.Continents can't be greater %f", MAX_VISIBILITY_DISTANCE - m_VisibleUnitGreyDistance);
        m_MaxVisibleDistanceOnContinents = MAX_VISIBILITY_DISTANCE - m_VisibleUnitGreyDistance;
    }

    // visibility in instances
    m_MaxVisibleDistanceInInstances        = sConfig.GetFloatDefault("Visibility.Distance.Instances",       DEFAULT_VISIBILITY_INSTANCE);
    if (m_MaxVisibleDistanceInInstances < 45 * getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO))
    {
        sLog.outError("Visibility.Distance.Instances can't be less max aggro radius %f", 45 * getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO));
        m_MaxVisibleDistanceInInstances = 45 * getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO);
    }
    else if (m_MaxVisibleDistanceInInstances + m_VisibleUnitGreyDistance >  MAX_VISIBILITY_DISTANCE)
    {
        sLog.outError("Visibility.Distance.Instances can't be greater %f", MAX_VISIBILITY_DISTANCE - m_VisibleUnitGreyDistance);
        m_MaxVisibleDistanceInInstances = MAX_VISIBILITY_DISTANCE - m_VisibleUnitGreyDistance;
    }

    // visibility in BG
    m_MaxVisibleDistanceInBG        = sConfig.GetFloatDefault("Visibility.Distance.BG",       DEFAULT_VISIBILITY_BG);
    if (m_MaxVisibleDistanceInBG < 45 * sWorld.getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO))
    {
        sLog.outError("Visibility.Distance.BG can't be less max aggro radius %f", 45 * getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO));
        m_MaxVisibleDistanceInBG = 45 * getConfig(CONFIG_FLOAT_RATE_CREATURE_AGGRO);
    }
    else if (m_MaxVisibleDistanceInBG + m_VisibleUnitGreyDistance >  MAX_VISIBILITY_DISTANCE)
    {
        sLog.outError("Visibility.Distance.BG can't be greater %f", MAX_VISIBILITY_DISTANCE - m_VisibleUnitGreyDistance);
        m_MaxVisibleDistanceInBG = MAX_VISIBILITY_DISTANCE - m_VisibleUnitGreyDistance;
    }

    m_MaxVisibleDistanceInFlight    = sConfig.GetFloatDefault("Visibility.Distance.InFlight",      DEFAULT_VISIBILITY_DISTANCE);
    if (m_MaxVisibleDistanceInFlight + m_VisibleObjectGreyDistance > MAX_VISIBILITY_DISTANCE)
    {
        sLog.outError("Visibility.Distance.InFlight can't be greater %f", MAX_VISIBILITY_DISTANCE - m_VisibleObjectGreyDistance);
        m_MaxVisibleDistanceInFlight = MAX_VISIBILITY_DISTANCE - m_VisibleObjectGreyDistance;
    }

    ///- Load the CharDelete related config options
    setConfigMinMax(CONFIG_UINT32_CHARDELETE_METHOD, "CharDelete.Method", 0, 0, 1);
    setConfigMinMax(CONFIG_UINT32_CHARDELETE_MIN_LEVEL, "CharDelete.MinLevel", 0, 0, getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL));
    setConfig(CONFIG_UINT32_CHARDELETE_KEEP_DAYS, "CharDelete.KeepDays", 30);

    if (configNoReload(reload, CONFIG_UINT32_GUID_RESERVE_SIZE_CREATURE, "GuidReserveSize.Creature", 100))
        setConfig(CONFIG_UINT32_GUID_RESERVE_SIZE_CREATURE,   "GuidReserveSize.Creature",   100);
    if (configNoReload(reload, CONFIG_UINT32_GUID_RESERVE_SIZE_GAMEOBJECT, "GuidReserveSize.GameObject", 100))
        setConfig(CONFIG_UINT32_GUID_RESERVE_SIZE_GAMEOBJECT, "GuidReserveSize.GameObject", 100);

    ///- Read the "Data" directory from the config file
    std::string dataPath = sConfig.GetStringDefault("DataDir", "./");

    // for empty string use current dir as for absent case
    if (dataPath.empty())
        dataPath = "./";
    // normalize dir path to path/ or path\ form
    else if (dataPath.at(dataPath.length() - 1) != '/' && dataPath.at(dataPath.length() - 1) != '\\')
        dataPath.append("/");

    if (reload)
    {
        if (dataPath != m_dataPath)
            sLog.outError("DataDir option can't be changed at mangosd.conf reload, using current value (%s).", m_dataPath.c_str());
    }
    else
    {
        m_dataPath = dataPath;
        sLog.outString("Using DataDir %s", m_dataPath.c_str());
    }

    setConfig(CONFIG_BOOL_VMAP_INDOOR_CHECK, "vmap.enableIndoorCheck", true);
    bool enableLOS = sConfig.GetBoolDefault("vmap.enableLOS", false);
    bool enableHeight = sConfig.GetBoolDefault("vmap.enableHeight", false);
    std::string ignoreSpellIds = sConfig.GetStringDefault("vmap.ignoreSpellIds", "");

    if (!enableHeight)
        sLog.outError("VMAP height use disabled! Creatures movements and other things will be in broken state.");

    VMAP::VMapFactory::createOrGetVMapManager()->setEnableLineOfSightCalc(enableLOS);
    VMAP::VMapFactory::createOrGetVMapManager()->setEnableHeightCalc(enableHeight);
    VMAP::VMapFactory::preventSpellsFromBeingTestedForLoS(ignoreSpellIds.c_str());
    sLog.outString("WORLD: VMap support included. LineOfSight:%i, getHeight:%i, indoorCheck:%i",
                   enableLOS, enableHeight, getConfig(CONFIG_BOOL_VMAP_INDOOR_CHECK) ? 1 : 0);
    sLog.outString("WORLD: VMap data directory is: %svmaps", m_dataPath.c_str());

    setConfig(CONFIG_BOOL_MMAP_ENABLED, "mmap.enabled", true);
    std::string ignoreMapIds = sConfig.GetStringDefault("mmap.ignoreMapIds", "");
    MMAP::MMapFactory::preventPathfindingOnMaps(ignoreMapIds.c_str());
    sLog.outString("WORLD: MMap pathfinding %sabled", getConfig(CONFIG_BOOL_MMAP_ENABLED) ? "en" : "dis");

    sLog.outString();
}

/// Initialize the World
void World::SetInitialWorldSettings()
{
    ///- Initialize the random number generator
    srand((unsigned int)time(NULL));

    ///- Time server startup
    uint32 uStartTime = WorldTimer::getMSTime();

    ///- Initialize detour memory management
    dtAllocSetCustom(dtCustomAlloc, dtCustomFree);

    ///- Initialize config settings
    LoadConfigSettings();
	sMod.ModInit();

    ///- Check the existence of the map files for all races start areas.
    if (!MapManager::ExistMapAndVMap(0, -6240.32f, 331.033f) ||                     // Dwarf/ Gnome
            !MapManager::ExistMapAndVMap(0, -8949.95f, -132.493f) ||                // Human
            !MapManager::ExistMapAndVMap(1, -618.518f, -4251.67f) ||                // Orc
            !MapManager::ExistMapAndVMap(0, 1676.35f, 1677.45f) ||                  // Scourge
            !MapManager::ExistMapAndVMap(1, 10311.3f, 832.463f) ||                  // NightElf
            !MapManager::ExistMapAndVMap(1, -2917.58f, -257.98f))                   // Tauren
    {
        sLog.outError("Correct *.map files not found in path '%smaps' or *.vmtree/*.vmtile files in '%svmaps'. Please place *.map and vmap files in appropriate directories or correct the DataDir value in the mangosd.conf file.", m_dataPath.c_str(), m_dataPath.c_str());
        Log::WaitBeforeContinueIfNeed();
        exit(1);
    }

    ///- Loading strings. Getting no records means core load has to be canceled because no error message can be output.
    sLog.outString("Loading MaNGOS strings...");
    if (!sObjectMgr.LoadMangosStrings())
    {
        Log::WaitBeforeContinueIfNeed();
        exit(1);                                            // Error message displayed in function already
    }

    ///- Update the realm entry in the database with the realm type from the config file
    // No SQL injection as values are treated as integers

    // not send custom type REALM_FFA_PVP to realm list
    uint32 server_type = IsFFAPvPRealm() ? uint32(REALM_TYPE_PVP) : getConfig(CONFIG_UINT32_GAME_TYPE);
    uint32 realm_zone = getConfig(CONFIG_UINT32_REALM_ZONE);
    LoginDatabase.PExecute("UPDATE realmlist SET icon = %u, timezone = %u WHERE id = '%u'", server_type, realm_zone, realmID);

    ///- Remove the bones (they should not exist in DB though) and old corpses after a restart
    CharacterDatabase.PExecute("DELETE FROM corpse WHERE corpse_type = '0' OR time < (UNIX_TIMESTAMP()-'%u')", 3 * DAY);

    ///- Load the DBC files
    sLog.outString("Initialize DBC data stores...");
    LoadDBCStores(m_dataPath);
    DetectDBCLang();
    sObjectMgr.SetDBCLocaleIndex(GetDefaultDbcLocale());    // Get once for all the locale index of DBC language (console/broadcasts)

    sLog.outString("Loading Script Names...");
    sScriptMgr.LoadScriptNames();

    sLog.outString("Loading WorldTemplate...");
    sObjectMgr.LoadWorldTemplate();

    sLog.outString("Loading InstanceTemplate...");
    sObjectMgr.LoadInstanceTemplate();

    sLog.outString("Loading SkillLineAbilityMultiMap Data...");
    sSpellMgr.LoadSkillLineAbilityMap();

    sLog.outString("Loading SkillRaceClassInfoMultiMap Data...");
    sSpellMgr.LoadSkillRaceClassInfoMap();

    ///- Clean up and pack instances
    sLog.outString("Cleaning up instances...");
    sMapPersistentStateMgr.CleanupInstances();              // must be called before `creature_respawn`/`gameobject_respawn` tables

    sLog.outString("Packing instances...");
    sMapPersistentStateMgr.PackInstances();

    sLog.outString("Packing groups...");
    sObjectMgr.PackGroupIds();                              // must be after CleanupInstances

    ///- Init highest guids before any guid using table loading to prevent using not initialized guids in some code.
    sObjectMgr.SetHighestGuids();                           // must be after packing instances
    sLog.outString();

    sLog.outString("Loading Page Texts...");
    sObjectMgr.LoadPageTexts();

    sLog.outString("Loading Game Object Templates...");     // must be after LoadPageTexts
    sObjectMgr.LoadGameobjectInfo();

    sLog.outString("Loading GameObject models...");
    LoadGameObjectModelList();
    sLog.outString();

    sLog.outString("Loading Spell Chain Data...");
    sSpellMgr.LoadSpellChains();

    sLog.outString("Loading Spell Elixir types...");
    sSpellMgr.LoadSpellElixirs();

    sLog.outString("Loading Spell Facing Flags...");
    sSpellMgr.LoadFacingCasterFlags();

    sLog.outString("Loading Spell Learn Skills...");
    sSpellMgr.LoadSpellLearnSkills();                       // must be after LoadSpellChains

    sLog.outString("Loading Spell Learn Spells...");
    sSpellMgr.LoadSpellLearnSpells();

    sLog.outString("Loading Spell Proc Event conditions...");
    sSpellMgr.LoadSpellProcEvents();

    sLog.outString("Loading Spell Bonus Data...");
    sSpellMgr.LoadSpellBonuses();

    sLog.outString("Loading Spell Proc Item Enchant...");
    sSpellMgr.LoadSpellProcItemEnchant();                   // must be after LoadSpellChains

    sLog.outString("Loading Aggro Spells Definitions...");
    sSpellMgr.LoadSpellThreats();

    sLog.outString("Loading NPC Texts...");
    sObjectMgr.LoadGossipText();

    sLog.outString("Loading Item Random Enchantments Table...");
    LoadRandomEnchantmentsTable();

    sLog.outString("Loading Item Templates...");            // must be after LoadRandomEnchantmentsTable and LoadPageTexts
    sObjectMgr.LoadItemPrototypes();

    sLog.outString("Loading Item Texts...");
    sObjectMgr.LoadItemTexts();

    sLog.outString("Loading Creature Model Based Info Data...");
    sObjectMgr.LoadCreatureModelInfo();

    sLog.outString("Loading Equipment templates...");
    sObjectMgr.LoadEquipmentTemplates();

    sLog.outString("Loading Creature Stats...");
    sObjectMgr.LoadCreatureClassLvlStats();

    sLog.outString("Loading Creature templates...");
    sObjectMgr.LoadCreatureTemplates();

    sLog.outString("Loading Creature template spells...");
    sObjectMgr.LoadCreatureTemplateSpells();

    sLog.outString("Loading SpellsScriptTarget...");
    sSpellMgr.LoadSpellScriptTarget();                      // must be after LoadCreatureTemplates and LoadGameobjectInfo

    sLog.outString("Loading ItemRequiredTarget...");
    sObjectMgr.LoadItemRequiredTarget();

    sLog.outString("Loading Reputation Reward Rates...");
    sObjectMgr.LoadReputationRewardRate();

    sLog.outString("Loading Creature Reputation OnKill Data...");
    sObjectMgr.LoadReputationOnKill();

    sLog.outString("Loading Reputation Spillover Data...");
    sObjectMgr.LoadReputationSpilloverTemplate();

    sLog.outString("Loading Points Of Interest Data...");
    sObjectMgr.LoadPointsOfInterest();

    sLog.outString("Loading Pet Create Spells...");
    sObjectMgr.LoadPetCreateSpells();

    sLog.outString("Loading Creature Data...");
    sObjectMgr.LoadCreatures();

    sLog.outString("Loading Creature Addon Data...");
    sObjectMgr.LoadCreatureAddons();                        // must be after LoadCreatureTemplates() and LoadCreatures()
    sLog.outString(">>> Creature Addon Data loaded");
    sLog.outString();

    sLog.outString("Loading Gameobject Data...");
    sObjectMgr.LoadGameObjects();

    sLog.outString("Loading CreatureLinking Data...");      // must be after Creatures
    sCreatureLinkingMgr.LoadFromDB();

    sLog.outString("Loading Objects Pooling Data...");
    sPoolMgr.LoadFromDB();

    sLog.outString("Loading Weather Data...");
    sWeatherMgr.LoadWeatherZoneChances();

    sLog.outString("Loading Quests...");
    sObjectMgr.LoadQuests();                                // must be loaded after DBCs, creature_template, item_template, gameobject tables

    sLog.outString("Loading Quests Relations...");
    sObjectMgr.LoadQuestRelations();                        // must be after quest load
    sLog.outString(">>> Quests Relations loaded");
    sLog.outString();

    sLog.outString("Loading Game Event Data...");           // must be after sPoolMgr.LoadFromDB and quests to properly load pool events and quests for events
    sGameEventMgr.LoadFromDB();
    sLog.outString(">>> Game Event Data loaded");
    sLog.outString();

    // Load Conditions
    sLog.outString("Loading Conditions...");
    sObjectMgr.LoadConditions();

    sLog.outString("Creating map persistent states for non-instanceable maps...");     // must be after PackInstances(), LoadCreatures(), sPoolMgr.LoadFromDB(), sGameEventMgr.LoadFromDB();
    sMapPersistentStateMgr.InitWorldMaps();
    sLog.outString();

    sLog.outString("Loading Creature Respawn Data...");     // must be after LoadCreatures(), and sMapPersistentStateMgr.InitWorldMaps()
    sMapPersistentStateMgr.LoadCreatureRespawnTimes();

    sLog.outString("Loading Gameobject Respawn Data...");   // must be after LoadGameObjects(), and sMapPersistentStateMgr.InitWorldMaps()
    sMapPersistentStateMgr.LoadGameobjectRespawnTimes();

    sLog.outString("Loading SpellArea Data...");            // must be after quest load
    sSpellMgr.LoadSpellAreas();

    sLog.outString("Loading AreaTrigger definitions...");
    sObjectMgr.LoadAreaTriggerTeleports();                  // must be after item template load

    sLog.outString("Loading Quest Area Triggers...");
    sObjectMgr.LoadQuestAreaTriggers();                     // must be after LoadQuests

    sLog.outString("Loading Tavern Area Triggers...");
    sObjectMgr.LoadTavernAreaTriggers();

    sLog.outString("Loading AreaTrigger script names...");
    sScriptMgr.LoadAreaTriggerScripts();

    sLog.outString("Loading event id script names...");
    sScriptMgr.LoadEventIdScripts();

    sLog.outString("Loading Graveyard-zone links...");
    sObjectMgr.LoadGraveyardZones();

    sLog.outString("Loading spell target destination coordinates...");
    sSpellMgr.LoadSpellTargetPositions();

    sLog.outString("Loading SpellAffect definitions...");
    sSpellMgr.LoadSpellAffects();

    sLog.outString("Loading spell pet auras...");
    sSpellMgr.LoadSpellPetAuras();

    sLog.outString("Loading Player Create Info & Level Stats...");
    sObjectMgr.LoadPlayerInfo();
    sLog.outString(">>> Player Create Info & Level Stats loaded");
    sLog.outString();

    sLog.outString("Loading Exploration BaseXP Data...");
    sObjectMgr.LoadExplorationBaseXP();

    sLog.outString("Loading Pet Name Parts...");
    sObjectMgr.LoadPetNames();

    CharacterDatabaseCleaner::CleanDatabase();
    sLog.outString();

    sLog.outString("Loading the max pet number...");
    sObjectMgr.LoadPetNumber();

    sLog.outString("Loading pet level stats...");
    sObjectMgr.LoadPetLevelInfo();

    sLog.outString("Loading Player Corpses...");
    sObjectMgr.LoadCorpses();

    sLog.outString("Loading Loot Tables...");
    LoadLootTables();
    sLog.outString(">>> Loot Tables loaded");
    sLog.outString();

    sLog.outString("Loading Skill Fishing base level requirements...");
    sObjectMgr.LoadFishingBaseSkillLevel();

    sLog.outString("Loading Npc Text Id...");
    sObjectMgr.LoadNpcGossips();                            // must be after load Creature and LoadGossipText

    sLog.outString("Loading Gossip scripts...");
    sScriptMgr.LoadGossipScripts();                         // must be before gossip menu options

    sObjectMgr.LoadGossipMenus();

    sLog.outString("Loading Vendors...");
    sObjectMgr.LoadVendorTemplates();                       // must be after load ItemTemplate
    sObjectMgr.LoadVendors();                               // must be after load CreatureTemplate, VendorTemplate, and ItemTemplate

    sLog.outString("Loading Trainers...");
    sObjectMgr.LoadTrainerTemplates();                      // must be after load CreatureTemplate
    sObjectMgr.LoadTrainers();                              // must be after load CreatureTemplate, TrainerTemplate

    sLog.outString("Loading Waypoint scripts...");          // before loading from creature_movement
    sScriptMgr.LoadCreatureMovementScripts();

    sLog.outString("Loading Waypoints...");
    sWaypointMgr.Load();

    sLog.outString("Loading ReservedNames...");
    sObjectMgr.LoadReservedPlayersNames();

    sLog.outString("Loading GameObjects for quests...");
    sObjectMgr.LoadGameObjectForQuests();

    sLog.outString("Loading BattleMasters...");
    sBattleGroundMgr.LoadBattleMastersEntry();

    sLog.outString("Loading BattleGround event indexes...");
    sBattleGroundMgr.LoadBattleEventIndexes();

    sLog.outString("Loading GameTeleports...");
    sObjectMgr.LoadGameTele();

    ///- Loading localization data
    sLog.outString("Loading Localization strings...");
    sObjectMgr.LoadCreatureLocales();                       // must be after CreatureInfo loading
    sObjectMgr.LoadGameObjectLocales();                     // must be after GameobjectInfo loading
    sObjectMgr.LoadItemLocales();                           // must be after ItemPrototypes loading
    sObjectMgr.LoadQuestLocales();                          // must be after QuestTemplates loading
    sObjectMgr.LoadGossipTextLocales();                     // must be after LoadGossipText
    sObjectMgr.LoadPageTextLocales();                       // must be after PageText loading
    sObjectMgr.LoadGossipMenuItemsLocales();                // must be after gossip menu items loading
    sObjectMgr.LoadPointOfInterestLocales();                // must be after POI loading
    sLog.outString(">>> Localization strings loaded");
    sLog.outString();

    ///- Load dynamic data tables from the database
    sLog.outString("Loading Auctions...");
    sAuctionMgr.LoadAuctionItems();
    sAuctionMgr.LoadAuctions();
    sLog.outString(">>> Auctions loaded");
    sLog.outString();

    sLog.outString("Loading Guilds...");
    sGuildMgr.LoadGuilds();

    sLog.outString("Loading Groups...");
    sObjectMgr.LoadGroups();

    sLog.outString("Returning old mails...");
    sObjectMgr.ReturnOrDeleteOldMails(false);

    sLog.outString("Loading GM tickets...");
    sTicketMgr.LoadGMTickets();

    ///- Load and initialize DBScripts Engine
    sLog.outString("Loading DB-Scripts Engine...");
    sScriptMgr.LoadQuestStartScripts();                     // must be after load Creature/Gameobject(Template/Data) and QuestTemplate
    sScriptMgr.LoadQuestEndScripts();                       // must be after load Creature/Gameobject(Template/Data) and QuestTemplate
    sScriptMgr.LoadSpellScripts();                          // must be after load Creature/Gameobject(Template/Data)
    sScriptMgr.LoadGameObjectScripts();                     // must be after load Creature/Gameobject(Template/Data)
    sScriptMgr.LoadGameObjectTemplateScripts();             // must be after load Creature/Gameobject(Template/Data)
    sScriptMgr.LoadEventScripts();                          // must be after load Creature/Gameobject(Template/Data)
    sScriptMgr.LoadCreatureDeathScripts();                  // must be after load Creature/Gameobject(Template/Data)
    sLog.outString(">>> Scripts loaded");
    sLog.outString();

    sLog.outString("Loading Scripts text locales...");      // must be after Load*Scripts calls
    sScriptMgr.LoadDbScriptStrings();

    ///- Load and initialize EventAI Scripts
    sLog.outString("Loading CreatureEventAI Texts...");
    sEventAIMgr.LoadCreatureEventAI_Texts(false);           // false, will checked in LoadCreatureEventAI_Scripts

    sLog.outString("Loading CreatureEventAI Summons...");
    sEventAIMgr.LoadCreatureEventAI_Summons(false);         // false, will checked in LoadCreatureEventAI_Scripts

    sLog.outString("Loading CreatureEventAI Scripts...");
    sEventAIMgr.LoadCreatureEventAI_Scripts();

    ///- Load and initialize scripting library
    sLog.outString("Initializing Scripting Library...");
    switch (sScriptMgr.LoadScriptLibrary(MANGOS_SCRIPT_NAME))
    {
        case SCRIPT_LOAD_OK:
            sLog.outString("Scripting library loaded.");
            break;
        case SCRIPT_LOAD_ERR_NOT_FOUND:
            sLog.outError("Scripting library not found or not accessible.");
            break;
        case SCRIPT_LOAD_ERR_WRONG_API:
            sLog.outError("Scripting library has wrong list functions (outdated?).");
            break;
        case SCRIPT_LOAD_ERR_OUTDATED:
            sLog.outError("Scripting library build for old mangosd revision. You need rebuild it.");
            break;
    }
    sLog.outString();

    ///- Initialize game time and timers
    sLog.outString("Initialize game time and timers");
    m_gameTime = time(NULL);
    m_startTime = m_gameTime;

    tm local;
    time_t curr;
    time(&curr);
    local = *(localtime(&curr));                            // dereference and assign
    char isoDate[128];
    sprintf(isoDate, "%04d-%02d-%02d %02d:%02d:%02d",
            local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);

    LoginDatabase.PExecute("INSERT INTO uptime (realmid, starttime, startstring, uptime) VALUES('%u', " UI64FMTD ", '%s', 0)",
                           realmID, uint64(m_startTime), isoDate);

	static uint32 abtimer = 0;
	abtimer = sConfig.GetIntDefault("AutoBroadcast.Timer", 30000);

    m_timers[WUPDATE_AUCTIONS].SetInterval(MINUTE * IN_MILLISECONDS);
    m_timers[WUPDATE_UPTIME].SetInterval(getConfig(CONFIG_UINT32_UPTIME_UPDATE)*MINUTE * IN_MILLISECONDS);
    // Update "uptime" table based on configuration entry in minutes.
    m_timers[WUPDATE_CORPSES].SetInterval(20 * MINUTE * IN_MILLISECONDS);
    m_timers[WUPDATE_DELETECHARS].SetInterval(DAY * IN_MILLISECONDS); // check for chars to delete every day
	m_timers[WUPDATE_AUTOBROADCAST].SetInterval(abtimer);

    // for AhBot
    m_timers[WUPDATE_AHBOT].SetInterval(20*IN_MILLISECONDS);// every 20 sec

	m_MaintenanceTimeChecker = sConfig.GetIntDefault("Maintenance.TimeChecker", 1000);
	battleground_kaiguan = 0;
	battleground_time_Start1 = GetBattleGroundTime(1, 2);
	battleground_time_End1 = GetBattleGroundTime(1, 3);
	battleground_time_Start2 = GetBattleGroundTime(3, 2);
	battleground_time_End2 = GetBattleGroundTime(3, 3);

	Worldsdebug = false;

    // to set mailtimer to return mails every day between 4 and 5 am
    // mailtimer is increased when updating auctions
    // one second is 1000 -(tested on win system)
    mail_timer = uint32((((localtime(&m_gameTime)->tm_hour + 20) % 24) * HOUR * IN_MILLISECONDS) / m_timers[WUPDATE_AUCTIONS].GetInterval());
    // 1440
    mail_timer_expires = uint32((DAY * IN_MILLISECONDS) / (m_timers[WUPDATE_AUCTIONS].GetInterval()));
    DEBUG_LOG("Mail timer set to: %u, mail return is called every %u minutes", mail_timer, mail_timer_expires);

    ///- Initialize static helper structures
    AIRegistry::Initialize();
    Player::InitVisibleBits();

    ///- Initialize MapManager
    sLog.outString("Starting Map System");
    sMapMgr.Initialize();
    sLog.outString();

    ///- Initialize Battlegrounds
    sLog.outString("Starting BattleGround System");
    sBattleGroundMgr.CreateInitialBattleGrounds();

    ///- Initialize Outdoor PvP
    sLog.outString("Starting Outdoor PvP System");
    sOutdoorPvPMgr.InitOutdoorPvP();

    // Not sure if this can be moved up in the sequence (with static data loading) as it uses MapManager
    sLog.outString("Loading Transports...");
    sMapMgr.LoadTransports();

    sLog.outString("Deleting expired bans...");
    LoginDatabase.Execute("DELETE FROM ip_banned WHERE unbandate<=UNIX_TIMESTAMP() AND unbandate<>bandate");
    sLog.outString();

    sLog.outString("Starting server Maintenance system...");
    InitServerMaintenanceCheck();

    sLog.outString("Loading Honor Standing list...");
    sObjectMgr.LoadStandingList();

    sLog.outString("Starting Game Event system...");
    uint32 nextGameEvent = sGameEventMgr.Initialize();
    m_timers[WUPDATE_EVENTS].SetInterval(nextGameEvent);    // depend on next event
    sLog.outString();

    sLog.outString("Loading grids for active creatures or transports...");
    sObjectMgr.LoadActiveEntities(NULL);
    sLog.outString();

    // Delete all characters which have been deleted X days before
    Player::DeleteOldCharacters();

	static SqlStatementID insertJiance;
	SqlStatement stmt = CharacterDatabase.CreateStatement(insertJiance, "DELETE FROM characters_battleground_jiance WHERE guid > ?");
	stmt.addUInt32(0);
	stmt.Execute();

    sLog.outString("Initialize AuctionHouseBot...");
    sAuctionBot.Initialize();
    sLog.outString();

    sLog.outString("---------------------------------------");
    sLog.outString("      CMANGOS: World initialized       ");
    sLog.outString("---------------------------------------");
    sLog.outString();

    uint32 uStartInterval = WorldTimer::getMSTimeDiff(uStartTime, WorldTimer::getMSTime());
    sLog.outString("SERVER STARTUP TIME: %i minutes %i seconds", uStartInterval / 60000, (uStartInterval % 60000) / 1000);
    sLog.outString();
}

void World::DetectDBCLang()
{
    uint32 m_lang_confid = sConfig.GetIntDefault("DBC.Locale", 255);

    if (m_lang_confid != 255 && m_lang_confid >= MAX_LOCALE)
    {
        sLog.outError("Incorrect DBC.Locale! Must be >= 0 and < %d (set to 0)", MAX_LOCALE);
        m_lang_confid = LOCALE_enUS;
    }

    ChrRacesEntry const* race = sChrRacesStore.LookupEntry(RACE_HUMAN);
    MANGOS_ASSERT(race);

    std::string availableLocalsStr;

    uint32 default_locale = MAX_LOCALE;
    for (int i = MAX_LOCALE - 1; i >= 0; --i)
    {
        if (strlen(race->name[i]) > 0)                      // check by race names
        {
            default_locale = i;
            m_availableDbcLocaleMask |= (1 << i);
            availableLocalsStr += localeNames[i];
            availableLocalsStr += " ";
        }
    }

    if (default_locale != m_lang_confid && m_lang_confid < MAX_LOCALE &&
            (m_availableDbcLocaleMask & (1 << m_lang_confid)))
    {
        default_locale = m_lang_confid;
    }

    if (default_locale >= MAX_LOCALE)
    {
        sLog.outError("Unable to determine your DBC Locale! (corrupt DBC?)");
        Log::WaitBeforeContinueIfNeed();
        exit(1);
    }

    m_defaultDbcLocale = LocaleConstant(default_locale);

    sLog.outString("Using %s DBC Locale as default. All available DBC locales: %s", localeNames[m_defaultDbcLocale], availableLocalsStr.empty() ? "<none>" : availableLocalsStr.c_str());
    sLog.outString();
}

/// Update the World !
void World::Update(uint32 diff)
{
    ///- Update the different timers
    for (int i = 0; i < WUPDATE_COUNT; ++i)
    {
        if (m_timers[i].GetCurrent() >= 0)
            m_timers[i].Update(diff);
        else
            m_timers[i].SetCurrent(0);
    }

    ///- Update the game time and check for shutdown time
    _UpdateGameTime();

    ///-Update mass mailer tasks if any
    sMassMailMgr.Update();

    /// <ul><li> Handle auctions when the timer has passed
    if (m_timers[WUPDATE_AUCTIONS].Passed())
    {
        m_timers[WUPDATE_AUCTIONS].Reset();

        ///- Update mails (return old mails with item, or delete them)
        //(tested... works on win)
        if (++mail_timer > mail_timer_expires)
        {
            mail_timer = 0;
            sObjectMgr.ReturnOrDeleteOldMails(true);
        }

        ///- Handle expired auctions
        sAuctionMgr.Update();
    }

    /// <li> Handle AHBot operations
    if (m_timers[WUPDATE_AHBOT].Passed())
    {
        sAuctionBot.Update();
        m_timers[WUPDATE_AHBOT].Reset();
    }

    /// <li> Handle session updates
    UpdateSessions(diff);

	// Update groups
	for (ObjectMgr::GroupMap::const_iterator itr = sObjectMgr.GetGroupSetBegin(); itr != sObjectMgr.GetGroupSetEnd(); ++itr)
		itr->second->Update();

    /// <li> Update uptime table
    if (m_timers[WUPDATE_UPTIME].Passed())
    {
        uint32 tmpDiff = uint32(m_gameTime - m_startTime);
        uint32 maxClientsNum = GetMaxActiveSessionCount();

        m_timers[WUPDATE_UPTIME].Reset();
        LoginDatabase.PExecute("UPDATE uptime SET uptime = %u, maxplayers = %u WHERE realmid = %u AND starttime = " UI64FMTD, tmpDiff, maxClientsNum, realmID, uint64(m_startTime));
    }

    /// <li> Handle all other objects
    ///- Update objects (maps, transport, creatures,...)
    sMapMgr.Update(diff);
    sBattleGroundMgr.Update(diff);
    sOutdoorPvPMgr.Update(diff);

    ///- Delete all characters which have been deleted X days before
    if (m_timers[WUPDATE_DELETECHARS].Passed())
    {
        m_timers[WUPDATE_DELETECHARS].Reset();
        Player::DeleteOldCharacters();
    }

    // execute callbacks from sql queries that were queued recently
    UpdateResultQueue();

    ///- Erase corpses once every 20 minutes
    if (m_timers[WUPDATE_CORPSES].Passed())
    {
        m_timers[WUPDATE_CORPSES].Reset();

        sObjectAccessor.RemoveOldCorpses();
    }

    ///- Process Game events when necessary
    if (m_timers[WUPDATE_EVENTS].Passed())
    {
        m_timers[WUPDATE_EVENTS].Reset();                   // to give time for Update() to be processed
        uint32 nextGameEvent = sGameEventMgr.Update();
        m_timers[WUPDATE_EVENTS].SetInterval(nextGameEvent);
        m_timers[WUPDATE_EVENTS].Reset();
    }

	static uint32 autobroadcaston = 0;
	autobroadcaston = sConfig.GetIntDefault("AutoBroadcast.On", 0);
	if (autobroadcaston == 1)
    {
		if (m_timers[WUPDATE_AUTOBROADCAST].Passed())
		{
			m_timers[WUPDATE_AUTOBROADCAST].Reset();
		    SendBroadcast();
		}
	}

    /// </ul>
    ///- Move all creatures with "delayed move" and remove and delete all objects with "delayed remove"
    sMapMgr.RemoveAllObjectsInRemoveList();

    // update the instance reset times
    sMapPersistentStateMgr.Update();

    if (GetDateToday() >= m_NextMaintenanceDate)
	{
		if (m_MaintenanceTimeChecker < diff)
		{
			sWorld.ShutdownServ(30, SHUTDOWN_MASK_RESTART, 2);
			LoginDatabase.PExecute("DELETE from character_queststatus WHERE quest = '%u'", 11055);
			LoginDatabase.PExecute("DELETE from character_queststatus WHERE quest = '%u'", 11056);
			LoginDatabase.PExecute("DELETE from character_queststatus WHERE quest = '%u'", 11063);
			//ServerMaintenanceStart();
			//sObjectMgr.LoadStandingList();
			m_MaintenanceTimeChecker = 600000;
		}
		else
			m_MaintenanceTimeChecker -= diff;
	}
 
	if (battleground_time_Start1 != 0 && battleground_kaiguan == 0)
	{
		if (battleground_time_Start1 <= GetGameTime())
		{
			LoginDatabase.PExecute("UPDATE characters_battleground SET battleground= '%u' WHERE id = '%u'", 1, 1);
			battleground_kaiguan = 1;
		}
	}
	if (battleground_time_End1 != 0 && battleground_kaiguan == 1)
	{
		if (battleground_time_End1 <= GetGameTime())
		{
				LoginDatabase.PExecute("UPDATE characters_battleground SET battleground= '%u' WHERE id = '%u'", 0, 1);
				battleground_kaiguan = 2;
		}
	}
	if (battleground_time_Start2 != 0 && battleground_kaiguan == 2)
	{
		if (battleground_time_Start2 <= GetGameTime())
		{
			LoginDatabase.PExecute("UPDATE characters_battleground SET battleground= '%u' WHERE id = '%u'", 1, 3);
			battleground_kaiguan = 3;
		}
	}
	if (battleground_time_End2 != 0 && battleground_kaiguan == 3)
	{
		if (battleground_time_End2 <= GetGameTime())
		{
			LoginDatabase.PExecute("UPDATE characters_battleground SET battleground= '%u', battlegroundtime_start = '%u', battlegroundtime_end = '%u' WHERE id = '%u'", 1, 1, 1, 1);
			LoginDatabase.PExecute("UPDATE characters_battleground SET battleground= '%u', battlegroundtime_start = '%u', battlegroundtime_end = '%u' WHERE id = '%u'", 1, 1, 1, 3);
			battleground_time_Start1 = 0;
			battleground_time_End1 = 0;
			battleground_time_Start2 = 0;
			battleground_time_End2 = 0;
			battleground_kaiguan = 0;
		}
	}
    // And last, but not least handle the issued cli commands
    ProcessCliCommands();

    // cleanup unused GridMap objects as well as VMaps
    sTerrainMgr.Update(diff);
}

namespace MaNGOS
{
    class WorldWorldTextBuilder
    {
        public:
            typedef std::vector<WorldPacket*> WorldPacketList;
            explicit WorldWorldTextBuilder(int32 textId, va_list* args = NULL) : i_textId(textId), i_args(args) {}
            void operator()(WorldPacketList& data_list, int32 loc_idx)
            {
                char const* text = sObjectMgr.GetMangosString(i_textId, loc_idx);

                if (i_args)
                {
                    // we need copy va_list before use or original va_list will corrupted
                    va_list ap;
                    va_copy(ap, *i_args);

                    char str [2048];
                    vsnprintf(str, 2048, text, ap);
                    va_end(ap);

                    do_helper(data_list, &str[0]);
                }
                else
                    do_helper(data_list, (char*)text);
            }
        private:
            char* lineFromMessage(char*& pos) { char* start = strtok(pos, "\n"); pos = NULL; return start; }
            void do_helper(WorldPacketList& data_list, char* text)
            {
                char* pos = text;

                while (char* line = lineFromMessage(pos))
                {
                    WorldPacket* data = new WorldPacket();
                    ChatHandler::BuildChatPacket(*data, CHAT_MSG_SYSTEM, line);
                    data_list.push_back(data);
                }
            }

            int32 i_textId;
            va_list* i_args;
    };
}                                                           // namespace MaNGOS

/// Sends a system message to all players
void World::SendWorldText(int32 string_id, ...)
{
    va_list ap;
    va_start(ap, string_id);

    MaNGOS::WorldWorldTextBuilder wt_builder(string_id, &ap);
    MaNGOS::LocalizedPacketListDo<MaNGOS::WorldWorldTextBuilder> wt_do(wt_builder);
    for (SessionMap::const_iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
    {
        if (WorldSession* session = itr->second)
        {
            Player* player = session->GetPlayer();
            if (player && player->IsInWorld())
                wt_do(player);
        }
    }

    va_end(ap);
}

void World::SendWorldTeamText(Player* pPlayer, int32 string_id, ...)
{
	va_list ap;
	va_start(ap, string_id);
	Team team = pPlayer->GetTeam();
	MaNGOS::WorldWorldTextBuilder wt_builder(string_id, &ap);
	MaNGOS::LocalizedPacketListDo<MaNGOS::WorldWorldTextBuilder> wt_do(wt_builder);
	for (SessionMap::const_iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
	{
		if (WorldSession* session = itr->second)
		{
			Player* player = session->GetPlayer();
			if (player && player->IsInWorld() && player->GetTeam() == team)
				wt_do(player);
		}
	}

	va_end(ap);
}

/// Sends a packet to all players with optional team and instance restrictions
void World::SendGlobalMessage(WorldPacket* packet)
{
    for (SessionMap::const_iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
    {
        if (WorldSession* session = itr->second)
        {
            Player* player = session->GetPlayer();
            if (player && player->IsInWorld())
                session->SendPacket(packet);
        }
    }
}

/// Sends a server message to the specified or all players
void World::SendServerMessage(ServerMessageType type, const char* text /*=""*/, Player* player /*= NULL*/)
{
    WorldPacket data(SMSG_SERVER_MESSAGE, 50);              // guess size
    data << uint32(type);
    data << text;

    if (player)
        player->GetSession()->SendPacket(&data);
    else
        SendGlobalMessage(&data);
}

/// Sends a zone under attack message to all players not in an instance
void World::SendZoneUnderAttackMessage(uint32 zoneId, Team team)
{
    WorldPacket data(SMSG_ZONE_UNDER_ATTACK, 4);
    data << uint32(zoneId);

    for (SessionMap::const_iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
    {
        if (WorldSession* session = itr->second)
        {
            Player* player = session->GetPlayer();
            if (player && player->IsInWorld() && player->GetTeam() == team && !player->GetMap()->Instanceable())
                itr->second->SendPacket(&data);
        }
    }
}

/// Sends a world defense message to all players not in an instance
void World::SendDefenseMessage(uint32 zoneId, int32 textId)
{
    for (SessionMap::const_iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
    {
        if (WorldSession* session = itr->second)
        {
            Player* player = session->GetPlayer();
            if (player && player->IsInWorld() && !player->GetMap()->Instanceable())
            {
                char const* message = session->GetMangosString(textId);
                uint32 messageLength = strlen(message) + 1;

                WorldPacket data(SMSG_DEFENSE_MESSAGE, 4 + 4 + messageLength);
                data << uint32(zoneId);
                data << uint32(messageLength);
                data << message;
                session->SendPacket(&data);
            }
        }
    }
}

/// Kick (and save) all players
void World::KickAll()
{
    m_QueuedSessions.clear();                               // prevent send queue update packet and login queued sessions

    // session not removed at kick and will removed in next update tick
    for (SessionMap::const_iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
        itr->second->KickPlayer();
}

/// Kick (and save) all players with security level less `sec`
void World::KickAllLess(AccountTypes sec)
{
    // session not removed at kick and will removed in next update tick
    for (SessionMap::const_iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
        if (WorldSession* session = itr->second)
            if (session->GetSecurity() < sec)
                session->KickPlayer();
}

/// Ban an account or ban an IP address, duration_secs if it is positive used, otherwise permban
BanReturn World::BanAccount(BanMode mode, std::string nameOrIP, uint32 duration_secs, std::string reason, const std::string& author)
{
    LoginDatabase.escape_string(nameOrIP);
    LoginDatabase.escape_string(reason);
    std::string safe_author = author;
    LoginDatabase.escape_string(safe_author);

    QueryResult* resultAccounts = NULL;                     // used for kicking

    ///- Update the database with ban information
    switch (mode)
    {
        case BAN_IP:
            // No SQL injection as strings are escaped
            resultAccounts = LoginDatabase.PQuery("SELECT id FROM account WHERE last_ip = '%s'", nameOrIP.c_str());
            LoginDatabase.PExecute("INSERT INTO ip_banned VALUES ('%s',UNIX_TIMESTAMP(),UNIX_TIMESTAMP()+%u,'%s','%s')", nameOrIP.c_str(), duration_secs, safe_author.c_str(), reason.c_str());
            break;
        case BAN_ACCOUNT:
            // No SQL injection as string is escaped
            resultAccounts = LoginDatabase.PQuery("SELECT id FROM account WHERE username = '%s'", nameOrIP.c_str());
            break;
        case BAN_CHARACTER:
            // No SQL injection as string is escaped
            resultAccounts = CharacterDatabase.PQuery("SELECT account FROM characters WHERE name = '%s'", nameOrIP.c_str());
            break;
        default:
            return BAN_SYNTAX_ERROR;
    }

    if (!resultAccounts)
    {
        if (mode == BAN_IP)
            return BAN_SUCCESS;                             // ip correctly banned but nobody affected (yet)
        else
            return BAN_NOTFOUND;                            // Nobody to ban
    }

    ///- Disconnect all affected players (for IP it can be several)
    do
    {
        Field* fieldsAccount = resultAccounts->Fetch();
        uint32 account = fieldsAccount->GetUInt32();

        if (mode != BAN_IP)
        {
            // No SQL injection as strings are escaped
            LoginDatabase.PExecute("INSERT INTO account_banned VALUES ('%u', UNIX_TIMESTAMP(), UNIX_TIMESTAMP()+%u, '%s', '%s', '1')",
                                   account, duration_secs, safe_author.c_str(), reason.c_str());
        }

        if (WorldSession* sess = FindSession(account))
            if (std::string(sess->GetPlayerName()) != author)
                sess->KickPlayer();
    }
    while (resultAccounts->NextRow());

    delete resultAccounts;
    return BAN_SUCCESS;
}

/// Remove a ban from an account or IP address
bool World::RemoveBanAccount(BanMode mode, std::string nameOrIP)
{
    if (mode == BAN_IP)
    {
        LoginDatabase.escape_string(nameOrIP);
        LoginDatabase.PExecute("DELETE FROM ip_banned WHERE ip = '%s'", nameOrIP.c_str());
    }
    else
    {
        uint32 account = 0;
        if (mode == BAN_ACCOUNT)
            account = sAccountMgr.GetId(nameOrIP);
        else if (mode == BAN_CHARACTER)
            account = sObjectMgr.GetPlayerAccountIdByPlayerName(nameOrIP);

        if (!account)
            return false;

        // NO SQL injection as account is uint32
        LoginDatabase.PExecute("UPDATE account_banned SET active = '0' WHERE id = '%u'", account);
    }
    return true;
}

/// Update the game time
void World::_UpdateGameTime()
{
    ///- update the time
    time_t thisTime = time(NULL);
    uint32 elapsed = uint32(thisTime - m_gameTime);
    m_gameTime = thisTime;

    ///- if there is a shutdown timer
    if (!m_stopEvent && m_ShutdownTimer > 0 && elapsed > 0)
    {
        ///- ... and it is overdue, stop the world (set m_stopEvent)
        if (m_ShutdownTimer <= elapsed)
        {
            if (!(m_ShutdownMask & SHUTDOWN_MASK_IDLE) || GetActiveAndQueuedSessionCount() == 0)
                m_stopEvent = true;                         // exist code already set
            else
                m_ShutdownTimer = 1;                        // minimum timer value to wait idle state
        }
        ///- ... else decrease it and if necessary display a shutdown countdown to the users
        else
        {
            m_ShutdownTimer -= elapsed;

            ShutdownMsg();
        }
    }
}

/// Shutdown the server
void World::ShutdownServ(uint32 time, uint32 options, uint8 exitcode)
{
    // ignore if server shutdown at next tick
    if (m_stopEvent)
        return;

    m_ShutdownMask = options;
    m_ExitCode = exitcode;

    ///- If the shutdown time is 0, set m_stopEvent (except if shutdown is 'idle' with remaining sessions)
    if (time == 0)
    {
        if (!(options & SHUTDOWN_MASK_IDLE) || GetActiveAndQueuedSessionCount() == 0)
            m_stopEvent = true;                             // exist code already set
        else
            m_ShutdownTimer = 1;                            // So that the session count is re-evaluated at next world tick
    }
    ///- Else set the shutdown timer and warn users
    else
    {
        m_ShutdownTimer = time;
        ShutdownMsg(true);
    }
}

/// Display a shutdown message to the user(s)
void World::ShutdownMsg(bool show /*= false*/, Player* player /*= NULL*/)
{
    // not show messages for idle shutdown mode
    if (m_ShutdownMask & SHUTDOWN_MASK_IDLE)
        return;

    ///- Display a message every 12 hours, 1 hour, 5 minutes, 1 minute and 15 seconds
    if (show ||
            (m_ShutdownTimer < 5 * MINUTE && (m_ShutdownTimer % 15) == 0) ||            // < 5 min; every 15 sec
            (m_ShutdownTimer < 15 * MINUTE && (m_ShutdownTimer % MINUTE) == 0) ||       // < 15 min; every 1 min
            (m_ShutdownTimer < 30 * MINUTE && (m_ShutdownTimer % (5 * MINUTE)) == 0) || // < 30 min; every 5 min
            (m_ShutdownTimer < 12 * HOUR && (m_ShutdownTimer % HOUR) == 0) ||           // < 12 h; every 1 h
            (m_ShutdownTimer >= 12 * HOUR && (m_ShutdownTimer % (12 * HOUR)) == 0))     // >= 12 h; every 12 h
    {
        std::string str = secsToTimeString(m_ShutdownTimer);

        ServerMessageType msgid = (m_ShutdownMask & SHUTDOWN_MASK_RESTART) ? SERVER_MSG_RESTART_TIME : SERVER_MSG_SHUTDOWN_TIME;

        SendServerMessage(msgid, str.c_str(), player);
        DEBUG_LOG("Server is %s in %s", (m_ShutdownMask & SHUTDOWN_MASK_RESTART ? "restart" : "shutting down"), str.c_str());
    }
}

/// Cancel a planned server shutdown
void World::ShutdownCancel()
{
    // nothing cancel or too later
    if (!m_ShutdownTimer || m_stopEvent)
        return;

    ServerMessageType msgid = (m_ShutdownMask & SHUTDOWN_MASK_RESTART) ? SERVER_MSG_RESTART_CANCELLED : SERVER_MSG_SHUTDOWN_CANCELLED;

    m_ShutdownMask = 0;
    m_ShutdownTimer = 0;
    m_ExitCode = SHUTDOWN_EXIT_CODE;                       // to default value
    SendServerMessage(msgid);

    DEBUG_LOG("Server %s cancelled.", (m_ShutdownMask & SHUTDOWN_MASK_RESTART ? "restart" : "shutdown"));
}

void World::UpdateSessions(uint32 diff)
{
    ///- Add new sessions
    WorldSession* sess;
    while (addSessQueue.next(sess))
        AddSession_(sess);

    ///- Then send an update signal to remaining ones
    for (SessionMap::iterator itr = m_sessions.begin(), next; itr != m_sessions.end(); itr = next)
    {
        next = itr;
        ++next;
        ///- and remove not active sessions from the list
        WorldSession* pSession = itr->second;
        WorldSessionFilter updater(pSession);

		if (!pSession->Update(diff, updater))
        {
            RemoveQueuedSession(pSession);
            m_sessions.erase(itr);
            delete pSession;
        }
    }
}

void World::PlayerWorldMailGuid(ItemPairs items, Player* pPlayer, std::string msgSubject, std::string msgText)
{
	uint32 cont = 0;
	for (SessionMap::iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
	{
		Player* pPlayera = itr->second->GetPlayer();
		if (!pPlayera)
			continue;

		if (!pPlayera->IsInWorld())
			continue;

		if (pPlayer == pPlayera)
			continue;

		if (pPlayera->getLevel() != 60)
			continue;

		if (cont == 0)
			cont = 1;

		MailDraft draft;
		draft.SetSubjectAndBody(msgSubject, msgText);
		for (ItemPairs::const_iterator itr = items.begin(); itr != items.end(); ++itr)
		{
			if (Item* item = Item::CreateItem(itr->first, itr->second, pPlayer))
			{
				item->SaveToDB();
				draft.AddItem(item);
			}
		}
		MailSender sender(MAIL_NORMAL, pPlayer->GetObjectGuid().GetCounter(), MAIL_STATIONERY_GM);
		draft.SendMailTo(MailReceiver(pPlayera, pPlayera->GetObjectGuid()), sender);
		ChatHandler(pPlayera).PSendSysMessage(LANG_MOXINGHUOQU_1, msgSubject);
	}
	if (cont == 1)
		ChatHandler(pPlayer).PSendSysMessage(LANG_MOXINGHUOQU_2, msgSubject);
	else
		ChatHandler(pPlayer).PSendSysMessage(LANG_MOXINGHUOQU_3);
}

void World::ServerMaintenanceStart()
{
    uint32 LastWeekEnd    = GetDateLastMaintenanceDay();
    m_NextMaintenanceDate   = LastWeekEnd + 7; // next maintenance begin

    if (m_NextMaintenanceDate <= GetDateToday())            // avoid loop in manually case, maybe useless
        m_NextMaintenanceDate += 7;

    // flushing rank points list ( standing must be reloaded after server maintenance )
    sObjectMgr.FlushRankPoints(LastWeekEnd);

    // save and update all online players
    for (SessionMap::iterator itr = m_sessions.begin(); itr != m_sessions.end(); ++itr)
        if (itr->second->GetPlayer() && itr->second->GetPlayer()->IsInWorld())
            itr->second->GetPlayer()->SaveToDB();

    CharacterDatabase.PExecute("UPDATE saved_variables SET NextMaintenanceDate = '"UI64FMTD"'", uint64(m_NextMaintenanceDate));
}

void World::InitServerMaintenanceCheck()
{
    QueryResult* result = CharacterDatabase.Query("SELECT NextMaintenanceDate FROM saved_variables");
    if (!result)
    {
        DEBUG_LOG("Maintenance date not found in SavedVariables, reseting it now.");
        uint32 mDate = GetDateLastMaintenanceDay();
        m_NextMaintenanceDate = mDate == GetDateToday() ?  mDate : mDate + 7;
        CharacterDatabase.PExecute("INSERT INTO saved_variables (NextMaintenanceDate) VALUES ('"UI64FMTD"')", uint64(m_NextMaintenanceDate));
    }
    else
    {
        m_NextMaintenanceDate = (*result)[0].GetUInt64();
        delete result;
    }

    if (m_NextMaintenanceDate <= GetDateToday())
        ServerMaintenanceStart();

    DEBUG_LOG("Server maintenance check initialized.");
}

uint32 World::GetBattleGroundTime(uint32 id, uint32 canshu)
{
	QueryResult* result = LoginDatabase.PQuery("SELECT battleground, battlegroundtime_start, battlegroundtime_end FROM characters_battleground WHERE id = %u", id);
	if (result)
	{
		Field *fields = result->Fetch();
		uint32 battleground = fields[0].GetUInt32();
		uint32 battlegroundtime_start = fields[1].GetUInt32();
		uint32 battlegroundtime_end = fields[2].GetUInt32();
		if (canshu == 1)
		{
			delete result;
			return battleground;
		}
		else
		if (canshu == 2)
		{
			delete result;
			return battlegroundtime_start;
		}
		else
		if (canshu == 3)
		{
			delete result;
			return battlegroundtime_end;
		}
	}
	delete result;
	return 0;
}

// This handles the issued and queued CLI/RA commands
void World::ProcessCliCommands()
{
    CliCommandHolder* command;
    while (cliCmdQueue.next(command))
    {
        DEBUG_LOG("CLI command under processing...");
        CliCommandHolder::Print* zprint = command->m_print;
        void* callbackArg = command->m_callbackArg;
        CliHandler handler(command->m_cliAccountId, command->m_cliAccessLevel, callbackArg, zprint);
        handler.ParseCommands(command->m_command);

        if (command->m_commandFinished)
            command->m_commandFinished(callbackArg, !handler.HasSentErrorMessage());

        delete command;
    }
}

void World::SendBroadcast()
 {
	std::string msg;
	static int nextid;
	
	QueryResult *result;
	if (nextid != 0)
	{
	    result = WorldDatabase.PQuery("SELECT `text`, `next` FROM `autobroadcast` WHERE `id` = %u", nextid);
	}
	else
	{
		result = WorldDatabase.PQuery("SELECT `text`, `next` FROM `autobroadcast` ORDER BY RAND() LIMIT 1");
	}
	
	if (!result)
		return;
	
	nextid = result->Fetch()[1].GetUInt8();
	msg = result->Fetch()[0].GetString();
	delete result;
	
	static uint32 abcenter = 0;
	abcenter = sConfig.GetIntDefault("AutoBroadcast.Center", 0);
	if (abcenter == 0)
	{
		sWorld.SendWorldText(LANG_AUTO_BROADCAST, msg.c_str());
		
		sLog.outString("AutoBroadcast: '%s'", msg.c_str());
	}
	if (abcenter == 1)
	{
		WorldPacket data(SMSG_NOTIFICATION, (msg.size() + 1));
		data << msg;
		sWorld.SendGlobalMessage(&data);
		
		sLog.outString("AutoBroadcast: '%s'", msg.c_str());
	}
	if (abcenter == 2)
	{
		sWorld.SendWorldText(LANG_AUTO_BROADCAST, msg.c_str());
		
		WorldPacket data(SMSG_NOTIFICATION, (msg.size() + 1));
		data << msg;
		sWorld.SendGlobalMessage(&data);
		
		sLog.outString("AutoBroadcast: '%s'", msg.c_str());
	}
}

void World::InitResultQueue()
{
}

void World::UpdateResultQueue()
{
    // process async result queues
    CharacterDatabase.ProcessResultQueue();
    WorldDatabase.ProcessResultQueue();
    LoginDatabase.ProcessResultQueue();
}

void World::UpdateRealmCharCount(uint32 accountId)
{
    CharacterDatabase.AsyncPQuery(this, &World::_UpdateRealmCharCount, accountId,
                                  "SELECT COUNT(guid) FROM characters WHERE account = '%u'", accountId);
}

void World::_UpdateRealmCharCount(QueryResult* resultCharCount, uint32 accountId)
{
    if (resultCharCount)
    {
        Field* fields = resultCharCount->Fetch();
        uint32 charCount = fields[0].GetUInt32();
        delete resultCharCount;

        LoginDatabase.BeginTransaction();
        LoginDatabase.PExecute("DELETE FROM realmcharacters WHERE acctid= '%u' AND realmid = '%u'", accountId, realmID);
        LoginDatabase.PExecute("INSERT INTO realmcharacters (numchars, acctid, realmid) VALUES (%u, %u, %u)", charCount, accountId, realmID);
        LoginDatabase.CommitTransaction();
    }
}

void World::SetPlayerLimit(int32 limit, bool needUpdate)
{
    if (limit < -SEC_ADMINISTRATOR)
        limit = -SEC_ADMINISTRATOR;

    // lock update need
    bool db_update_need = needUpdate || (limit < 0) != (m_playerLimit < 0) || (limit < 0 && m_playerLimit < 0 && limit != m_playerLimit);

    m_playerLimit = limit;

    if (db_update_need)
        LoginDatabase.PExecute("UPDATE realmlist SET allowedSecurityLevel = '%u' WHERE id = '%u'",
                               uint32(GetPlayerSecurityLimit()), realmID);
}

void World::UpdateMaxSessionCounters()
{
    m_maxActiveSessionCount = std::max(m_maxActiveSessionCount, uint32(m_sessions.size() - m_QueuedSessions.size()));
    m_maxQueuedSessionCount = std::max(m_maxQueuedSessionCount, uint32(m_QueuedSessions.size()));
}

void World::LoadDBVersion()
{
    QueryResult* result = WorldDatabase.Query("SELECT version, creature_ai_version FROM db_version LIMIT 1");
    if (result)
    {
        Field* fields = result->Fetch();

        m_DBVersion              = fields[0].GetCppString();
        m_CreatureEventAIVersion = fields[1].GetCppString();

        delete result;
    }

    if (m_DBVersion.empty())
        m_DBVersion = "Unknown world database.";

    if (m_CreatureEventAIVersion.empty())
        m_CreatureEventAIVersion = "Unknown creature EventAI.";
}

void World::setConfig(eConfigUInt32Values index, char const* fieldname, uint32 defvalue)
{
    setConfig(index, sConfig.GetIntDefault(fieldname, defvalue));
    if (int32(getConfig(index)) < 0)
    {
        sLog.outError("%s (%i) can't be negative. Using %u instead.", fieldname, int32(getConfig(index)), defvalue);
        setConfig(index, defvalue);
    }
}

void World::setConfig(eConfigInt32Values index, char const* fieldname, int32 defvalue)
{
    setConfig(index, sConfig.GetIntDefault(fieldname, defvalue));
}

void World::setConfig(eConfigFloatValues index, char const* fieldname, float defvalue)
{
    setConfig(index, sConfig.GetFloatDefault(fieldname, defvalue));
}

void World::setConfig(eConfigBoolValues index, char const* fieldname, bool defvalue)
{
    setConfig(index, sConfig.GetBoolDefault(fieldname, defvalue));
}

void World::setConfigPos(eConfigFloatValues index, char const* fieldname, float defvalue)
{
    setConfig(index, fieldname, defvalue);
    if (getConfig(index) < 0.0f)
    {
        sLog.outError("%s (%f) can't be negative. Using %f instead.", fieldname, getConfig(index), defvalue);
        setConfig(index, defvalue);
    }
}

void World::setConfigMin(eConfigUInt32Values index, char const* fieldname, uint32 defvalue, uint32 minvalue)
{
    setConfig(index, fieldname, defvalue);
    if (getConfig(index) < minvalue)
    {
        sLog.outError("%s (%u) must be >= %u. Using %u instead.", fieldname, getConfig(index), minvalue, minvalue);
        setConfig(index, minvalue);
    }
}

void World::setConfigMin(eConfigInt32Values index, char const* fieldname, int32 defvalue, int32 minvalue)
{
    setConfig(index, fieldname, defvalue);
    if (getConfig(index) < minvalue)
    {
        sLog.outError("%s (%i) must be >= %i. Using %i instead.", fieldname, getConfig(index), minvalue, minvalue);
        setConfig(index, minvalue);
    }
}

void World::setConfigMin(eConfigFloatValues index, char const* fieldname, float defvalue, float minvalue)
{
    setConfig(index, fieldname, defvalue);
    if (getConfig(index) < minvalue)
    {
        sLog.outError("%s (%f) must be >= %f. Using %f instead.", fieldname, getConfig(index), minvalue, minvalue);
        setConfig(index, minvalue);
    }
}

void World::setConfigMinMax(eConfigUInt32Values index, char const* fieldname, uint32 defvalue, uint32 minvalue, uint32 maxvalue)
{
    setConfig(index, fieldname, defvalue);
    if (getConfig(index) < minvalue)
    {
        sLog.outError("%s (%u) must be in range %u...%u. Using %u instead.", fieldname, getConfig(index), minvalue, maxvalue, minvalue);
        setConfig(index, minvalue);
    }
    else if (getConfig(index) > maxvalue)
    {
        sLog.outError("%s (%u) must be in range %u...%u. Using %u instead.", fieldname, getConfig(index), minvalue, maxvalue, maxvalue);
        setConfig(index, maxvalue);
    }
}

void World::setConfigMinMax(eConfigInt32Values index, char const* fieldname, int32 defvalue, int32 minvalue, int32 maxvalue)
{
    setConfig(index, fieldname, defvalue);
    if (getConfig(index) < minvalue)
    {
        sLog.outError("%s (%i) must be in range %i...%i. Using %i instead.", fieldname, getConfig(index), minvalue, maxvalue, minvalue);
        setConfig(index, minvalue);
    }
    else if (getConfig(index) > maxvalue)
    {
        sLog.outError("%s (%i) must be in range %i...%i. Using %i instead.", fieldname, getConfig(index), minvalue, maxvalue, maxvalue);
        setConfig(index, maxvalue);
    }
}

void World::setConfigMinMax(eConfigFloatValues index, char const* fieldname, float defvalue, float minvalue, float maxvalue)
{
    setConfig(index, fieldname, defvalue);
    if (getConfig(index) < minvalue)
    {
        sLog.outError("%s (%f) must be in range %f...%f. Using %f instead.", fieldname, getConfig(index), minvalue, maxvalue, minvalue);
        setConfig(index, minvalue);
    }
    else if (getConfig(index) > maxvalue)
    {
        sLog.outError("%s (%f) must be in range %f...%f. Using %f instead.", fieldname, getConfig(index), minvalue, maxvalue, maxvalue);
        setConfig(index, maxvalue);
    }
}

bool World::configNoReload(bool reload, eConfigUInt32Values index, char const* fieldname, uint32 defvalue)
{
    if (!reload)
        return true;

    uint32 val = sConfig.GetIntDefault(fieldname, defvalue);
    if (val != getConfig(index))
        sLog.outError("%s option can't be changed at mangosd.conf reload, using current value (%u).", fieldname, getConfig(index));

    return false;
}

bool World::configNoReload(bool reload, eConfigInt32Values index, char const* fieldname, int32 defvalue)
{
    if (!reload)
        return true;

    int32 val = sConfig.GetIntDefault(fieldname, defvalue);
    if (val != getConfig(index))
        sLog.outError("%s option can't be changed at mangosd.conf reload, using current value (%i).", fieldname, getConfig(index));

    return false;
}

bool World::configNoReload(bool reload, eConfigFloatValues index, char const* fieldname, float defvalue)
{
    if (!reload)
        return true;

    float val = sConfig.GetFloatDefault(fieldname, defvalue);
    if (val != getConfig(index))
        sLog.outError("%s option can't be changed at mangosd.conf reload, using current value (%f).", fieldname, getConfig(index));

    return false;
}

bool World::configNoReload(bool reload, eConfigBoolValues index, char const* fieldname, bool defvalue)
{
    if (!reload)
        return true;

    bool val = sConfig.GetBoolDefault(fieldname, defvalue);
    if (val != getConfig(index))
        sLog.outError("%s option can't be changed at mangosd.conf reload, using current value (%s).", fieldname, getConfig(index) ? "'true'" : "'false'");

    return false;
}

void World::InvalidatePlayerDataToAllClient(ObjectGuid guid)
{
    WorldPacket data(SMSG_INVALIDATE_PLAYER, 8);
    data << guid;
    SendGlobalMessage(&data);
}

void World::SetHuoLia(time_t t, time_t now, time_t nextWeek, time_t today, uint32 period, uint32 diff)
{
	if (t < now || t > nextWeek)
	{
		t = (t / DAY) * DAY;
		t += (((today - t) / period + 1) * period + diff);
		uint32 TimeStart1 = 0;
		uint32 TimeEnd1 = 0;
		uint32 TimeStart2 = 0;
		uint32 TimeEnd2 = 0;
		if (sWorld.GetDateToday() == sWorld.GetDateLastMaintenanceDayXp1() || sWorld.GetDateToday() == sWorld.GetDateLastMaintenanceDayXp2())
		{
			TimeStart1 = (uint32)t - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_START1_1);
			TimeEnd1 = (uint32)t - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_END1_1);
			TimeStart2 = (uint32)t - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_START2_2);
			TimeEnd2 = (uint32)t - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_END2_2);
		}
		else
		{
			TimeStart1 = (uint32)t - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_START_1);
			TimeEnd1 = (uint32)t - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_END_1);
			TimeStart2 = (uint32)t - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_START_2);
			TimeEnd2 = (uint32)t - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_END_2);
		}
		CharacterDatabase.DirectPExecute("UPDATE characters_battleground SET battleground = '%u', battlegroundtime_start = '%u', battlegroundtime_end = '%u' WHERE id = '%u'", 0, TimeStart1, TimeEnd1, 1);
		CharacterDatabase.DirectPExecute("UPDATE characters_battleground SET battleground = '%u', battlegroundtime_start = '%u', battlegroundtime_end = '%u' WHERE id = '%u'", 0, TimeStart2, TimeEnd2, 3);
	}
}

void World::SetHuoLib(uint64 next_reset)
{
	sObjectAccessor.SaveAllPlayers();
	uint32 TimeStart1 = 0;
	uint32 TimeEnd1 = 0;
	uint32 TimeStart2 = 0;
	uint32 TimeEnd2 = 0;
	if (sWorld.GetDateToday() == sWorld.GetDateLastMaintenanceDayXp1() || sWorld.GetDateToday() == sWorld.GetDateLastMaintenanceDayXp2())
	{
		TimeStart1 = (uint32)next_reset - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_START1_1);
		TimeEnd1 = (uint32)next_reset - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_END1_1);
		TimeStart2 = (uint32)next_reset - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_START2_2);
		TimeEnd2 = (uint32)next_reset - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_END2_2);
	}
	else
	{
		TimeStart1 = (uint32)next_reset - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_START_1);
		TimeEnd1 = (uint32)next_reset - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_END_1);
		TimeStart2 = (uint32)next_reset - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_START_2);
		TimeEnd2 = (uint32)next_reset - 86400 + sWorld.getConfig(CONFIG_UINT32_HONORAD_TIME_END_2);
	}
	CharacterDatabase.DirectPExecute("UPDATE characters_battleground SET battleground = '%u', battlegroundtime_start = '%u', battlegroundtime_end = '%u' WHERE id = '%u'", 0, TimeStart1, TimeEnd1, 1);
	CharacterDatabase.DirectPExecute("UPDATE characters_battleground SET battleground = '%u', battlegroundtime_start = '%u', battlegroundtime_end = '%u' WHERE id = '%u'", 0, TimeStart2, TimeEnd2, 3);
	sWorld.battleground_time_Start1 = TimeStart1;
	sWorld.battleground_time_End1 = TimeEnd1;
	sWorld.battleground_time_Start2 = TimeStart2;
	sWorld.battleground_time_End2 = TimeEnd2;
}

bool World::BattleGroundWeek()
{
	uint32 time_begin = 1440518400;
	uint32 time_diff = time(NULL) - time_begin;
	uint32 time = int(time_diff / 3600 / 24 / 7) % 2;
	if (time == 0)
		return true;

	return false;
}

uint32 World::GetTaqWuZhi(uint32 id, uint32 canshu)
{
	QueryResult* result = LoginDatabase.PQuery("SELECT current_no, target_no FROM item_taq_wuzhi WHERE id = %u", id);
	if (result)
	{
		Field *fields = result->Fetch();
		uint32 current_no = fields[0].GetUInt32();
		uint32 target_no = fields[1].GetUInt32();

		if (canshu == 1)
		{
			delete result;
			return current_no;
		}
		else
		if (canshu == 2)
		{
			delete result;
			return target_no;
		}
	}
	delete result;
	return 0;
}

void World::SetTaqWuZhi(uint32 id, uint32 cont)
{
	CharacterDatabase.PExecute("UPDATE item_taq_wuzhi SET current_no = current_no + %u WHERE id = %u", cont, id);
	CharacterDatabase.CommitTransaction();
}

bool World::ChaXunGuild_IdBossFd(uint32 Entry)
{
	QueryResult* result = LoginDatabase.PQuery("SELECT guildid FROM _kx_fdgonggao WHERE BoddEntry = %u", Entry);
	if (!result)
		return false;

	delete result;
	return true;
}

bool World::ChaXunTeam_IdBossFd(uint32 Entry, uint32 Team_Id)
{
	QueryResult* result = LoginDatabase.PQuery("SELECT team FROM _kx_fdgonggao WHERE BoddEntry = %u", Entry);
	if (result)
	{
		do
		{
			Field *fields = result->Fetch();
			uint32 guildid = fields[0].GetUInt32();
			if (guildid == Team_Id)
				return true;

		} 
		while (result->NextRow());
		delete result;
	}
	return false;
}

bool World::WorldBossId(uint32 Entry)
{
	QueryResult* result = LoginDatabase.PQuery("SELECT id FROM _kx_fdboss WHERE BoddEntry = %u", Entry);
	if (!result)
		return false;

	delete result;
	return true;
}

void World::SetBossId(uint32 Entry)
{
	static SqlStatementID insertAuras;
	SqlStatement stmt = CharacterDatabase.CreateStatement(insertAuras, "INSERT INTO _kx_fdboss (BoddEntry) VALUES (?)");
	stmt.addUInt32(Entry);
	stmt.Execute();
}

uint32 World::ChaXunBossFdId()
{
	uint32 cont = 0;
	QueryResult* result = LoginDatabase.PQuery("SELECT BoddEntry FROM _kx_fdgonggao WHERE id >= %u", 1);
	if (result)
	{
		do
		{
			Field *fields = result->Fetch();
			uint32 BoddEntry = fields[0].GetUInt32();
			++cont;

		} 
		while (result->NextRow());
		delete result;
	}

	return cont;
}

void World::SetBossFd(uint32 Entry, uint32 guildid, uint32 world_fd, uint32 lm_fd, uint32 bl_fd, uint32 team)
{
	static SqlStatementID insertAuras;
	SqlStatement stmt = CharacterDatabase.CreateStatement(insertAuras, "INSERT INTO _kx_fdgonggao (BoddEntry, guildid, world_fd, lm_fd, bl_fd, team) VALUES (?, ?, ?, ?, ?, ?)");
	stmt.addUInt32(Entry);
	stmt.addUInt32(guildid);
	stmt.addUInt32(world_fd);
	stmt.addUInt32(lm_fd);
	stmt.addUInt32(bl_fd);
	stmt.addUInt32(team);
	stmt.Execute();
}
