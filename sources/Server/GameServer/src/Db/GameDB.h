#include <list>
#include "Util.h"
#include "Database.h"
#include "App/GameAppNet.h"
#include "Player/Player.h"

#define defCHA_TABLE_VER		110
#define defCHA_TABLE_NEW_VER	111

enum ESaveType {
	enumSAVE_TYPE_OFFLINE, //
	enumSAVE_TYPE_SWITCH, //
	enumSAVE_TYPE_TIMER, //
	enumSAVE_TYPE_TRADE, //
};

class CPlayer;

// ============================================================================
// Row structures — маппинг 1:1 на строки SQL-таблиц
// ============================================================================

struct AccountRow {
	std::int32_t ato_id;
	std::string ato_nome;
	std::int32_t jmes;
	std::string ator_ids;
	std::string last_ip;
	std::string disc_reason;
	std::string last_leave;
	std::string password;
	std::int32_t merge_state;
	std::int32_t IMP;
	std::int32_t total_votes;
	std::int32_t credit;
};

struct AmphitheaterSettingRow {
	std::int32_t section;
	std::int32_t season;
	std::int32_t round;
	std::int32_t state;
	std::string createdate;
	std::string updatetime;
};

struct AmphitheaterTeamRow {
	std::int32_t id;
	std::int32_t captain;
	std::string member;
	std::int32_t matchno;
	std::int32_t state;
	std::int32_t map;
	std::int32_t mapflag;
	std::int32_t winnum;
	std::int32_t losenum;
	std::int32_t relivenum;
	std::string createdate;
	std::string updatetime;
};

struct BoatRow {
	std::int32_t boat_id;
	std::int32_t boat_berth;
	std::string boat_name;
	std::int32_t boat_boatid;
	std::int32_t boat_header;
	std::int32_t boat_body;
	std::int32_t boat_engine;
	std::int32_t boat_cannon;
	std::int32_t boat_equipment;
	std::int32_t boat_bagsize;
	std::string boat_bag;
	std::int32_t boat_diecount;
	std::string boat_isdead;
	std::int32_t cur_endure;
	std::int32_t mx_endure;
	std::int32_t cur_supply;
	std::int32_t mx_supply;
	std::string skill_state;
	std::int32_t boat_ownerid;
	std::string boat_createtime;
	std::string boat_isdeleted;
	std::string map;
	std::int32_t map_x;
	std::int32_t map_y;
	std::int32_t angle;
	std::int32_t degree;
	std::int32_t exp;
	std::int32_t version;
};

struct CharacterRow {
	std::int32_t atorID;
	std::string atorNome;
	std::string motto;
	std::int32_t icon;
	std::int32_t version;
	std::int32_t pk_ctrl;
	std::int32_t endeMem;
	std::int32_t ato_id;
	std::int32_t guild_id;
	std::int32_t guild_stat;
	std::int64_t guild_permission;
	std::string job;
	std::int32_t degree;
	std::int64_t exp;
	std::int32_t hp;
	std::int32_t sp;
	std::int32_t ap;
	std::int32_t tp;
	std::int32_t bomd;
	std::int32_t str, dex, agi, con, sta, luk;
	std::int32_t sail_lv;
	std::int32_t sail_exp;
	std::int32_t sail_left_exp;
	std::int32_t live_lv;
	std::int32_t live_exp;
	std::string map;
	std::int32_t map_x;
	std::int32_t map_y;
	std::int32_t radius;
	std::int32_t angle;
	std::string olhe;
	std::int32_t kb_capacity;
	std::string kitbag;
	std::string skillbag;
	std::string shortcut;
	std::string mission;
	std::string misrecord;
	std::string mistrigger;
	std::string miscount;
	std::string birth;
	std::string login_cha;
	std::int32_t live_tp;
	std::int32_t delflag;
	std::string operdate;
	std::string deldate;
	std::string main_map;
	std::string skill_state;
	std::string bank;
	std::string estop;
	std::int32_t estoptime;
	std::int32_t kb_locked;
	std::int32_t kitbag_tmp;
	std::int32_t credit;
	std::int32_t store_item;
	std::string extend;
	std::int32_t chatColour;
	std::int32_t IMP;
};

struct CharacterLogRow {
	std::int32_t atorID;
	std::string atorNome;
	std::int32_t ato_id;
	std::int32_t guild_id;
	std::string job;
	std::int32_t degree;
	std::int32_t exp;
	std::int32_t hp, sp, ap, tp, bomd;
	std::int32_t str, dex, agi, con, sta, luk;
	std::string map;
	std::int32_t map_x, map_y, radius;
	std::string olhe;
	std::string del_date;
};

struct FriendsRow {
	std::int32_t cha_id1;
	std::int32_t cha_id2;
	std::string relation;
	std::string createtime;
};

struct GuildRow {
	std::int32_t guild_id;
	std::string guild_name;
	std::string motto;
	std::string passwd;
	std::int32_t leader_id;
	std::int64_t exp;
	std::int64_t gold;
	std::string bank;
	std::int32_t level;
	std::int32_t member_total;
	std::int32_t try_total;
	std::string disband_date;
	std::int32_t challlevel;
	std::int32_t challid;
	std::int64_t challmoney;
	std::int32_t challstart;
};

struct LotterySettingRow {
	std::int32_t section;
	std::int32_t issue;
	std::int32_t state;
	std::string createdate;
	std::string updatetime;
	std::string itemno;
};

struct MasterRow {
	std::int32_t cha_id1;
	std::int32_t cha_id2;
	std::int32_t finish;
	std::string relation;
};

struct ParamRow {
	std::int32_t id;
	std::int32_t param1, param2, param3, param4, param5;
	std::int32_t param6, param7, param8, param9, param10;
};

struct PersonAvatarRow {
	std::int32_t atorID;
	std::vector<uint8_t> avatar;
};

struct PersonInfoRow {
	std::int32_t atorID;
	std::string motto;
	std::int32_t showmotto;
	std::string sex;
	std::int32_t age;
	std::string name;
	std::string animal_zodiac;
	std::string blood_type;
	std::int32_t birthday;
	std::string state;
	std::string city;
	std::string constellation;
	std::string career;
	std::int32_t avatarsize;
	std::int32_t prevent;
	std::int32_t support;
	std::int32_t oppose;
};

struct PropertyRow {
	std::int64_t id;
	std::int64_t atorID;
	std::string context;
	std::int64_t sum;
	std::string time;
};

struct ResourceRow {
	std::int32_t id;
	std::int32_t atorID;
	std::int32_t type_id;
	std::string content;
};

struct StatLogRow {
	std::string track_date;
	std::int32_t login_num;
	std::int32_t play_num;
	std::int32_t wgplay_num;
};

struct StatDegreeRow {
	std::string statDate;
	std::int32_t degree;
	std::int64_t characterCount;
};

struct StatGenderRow {
	std::string statDate;
	std::string gender;
	std::int64_t genderCount;
};

struct StatJobRow {
	std::string statDate;
	std::string job;
	std::int64_t characterCount;
};

struct StatLoginRow {
	std::string statDate;
	std::int64_t loginCount;
};

struct StatMapRow {
	std::string statDate;
	std::string map;
	std::int64_t playCount;
};

struct TicketRow {
	std::int32_t id;
	std::int32_t atorID;
	std::int32_t issue;
	std::string itemno;
	std::int32_t real;
	std::string buydate;
};

struct TradeLogRow {
	std::int32_t ID;
	std::string ExecuteTime;
	std::string GameServer;
	std::string Action;
	std::string From;
	std::string To;
	std::string Memo;
};

struct WeekReportRow {
	std::string ato_nome;
	std::string atorNome;
	std::int32_t degree;
	std::string ip;
	std::string createdate;
	std::string logouttime;
	std::int32_t playtime;
	std::string Guild_Name;
};

struct WinTicketRow {
	std::int32_t issue;
	std::string itemno;
	std::int32_t grade;
	std::string createdate;
	std::int32_t num;
};

// ============================================================================
// Типизированные таблицы — наследники Corsairs::Util::OdbcTable<T> (Column DSL)
// ============================================================================

class TableAccount : public Corsairs::Util::OdbcTable<AccountRow> {
public:
	explicit TableAccount(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "account", {
			Corsairs::Util::MakeColumn("ato_id",       &AccountRow::ato_id, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("ato_nome",     &AccountRow::ato_nome),
			Corsairs::Util::MakeColumn("jmes",         &AccountRow::jmes),
			Corsairs::Util::MakeColumn("ator_ids",     &AccountRow::ator_ids),
			Corsairs::Util::MakeColumn("last_ip",      &AccountRow::last_ip),
			Corsairs::Util::MakeColumn("disc_reason",  &AccountRow::disc_reason),
			Corsairs::Util::MakeColumn("last_leave",   &AccountRow::last_leave, Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("password",     &AccountRow::password),
			Corsairs::Util::MakeColumn("merge_state",  &AccountRow::merge_state),
			Corsairs::Util::MakeColumn("IMP",          &AccountRow::IMP),
			Corsairs::Util::MakeColumn("total_votes",  &AccountRow::total_votes),
			Corsairs::Util::MakeColumn("credit",       &AccountRow::credit),
		}) {}
};

class TableAmphitheaterSetting : public Corsairs::Util::OdbcTable<AmphitheaterSettingRow> {
public:
	explicit TableAmphitheaterSetting(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "AmphitheaterSetting", {
			Corsairs::Util::MakeColumn("section",    &AmphitheaterSettingRow::section),
			Corsairs::Util::MakeColumn("season",     &AmphitheaterSettingRow::season),
			Corsairs::Util::MakeColumn("round",      &AmphitheaterSettingRow::round),
			Corsairs::Util::MakeColumn("state",      &AmphitheaterSettingRow::state),
			Corsairs::Util::MakeColumn("createdate", &AmphitheaterSettingRow::createdate, Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("updatetime", &AmphitheaterSettingRow::updatetime, Corsairs::Util::Nullable | Corsairs::Util::Timestamp),
		}) {}
};

class TableAmphitheaterTeam : public Corsairs::Util::OdbcTable<AmphitheaterTeamRow> {
public:
	explicit TableAmphitheaterTeam(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "AmphitheaterTeam", {
			Corsairs::Util::MakeColumn("id",         &AmphitheaterTeamRow::id, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("captain",    &AmphitheaterTeamRow::captain),
			Corsairs::Util::MakeColumn("member",     &AmphitheaterTeamRow::member),
			Corsairs::Util::MakeColumn("matchno",    &AmphitheaterTeamRow::matchno),
			Corsairs::Util::MakeColumn("state",      &AmphitheaterTeamRow::state),
			Corsairs::Util::MakeColumn("map",        &AmphitheaterTeamRow::map),
			Corsairs::Util::MakeColumn("mapflag",    &AmphitheaterTeamRow::mapflag),
			Corsairs::Util::MakeColumn("winnum",     &AmphitheaterTeamRow::winnum),
			Corsairs::Util::MakeColumn("losenum",    &AmphitheaterTeamRow::losenum),
			Corsairs::Util::MakeColumn("relivenum",  &AmphitheaterTeamRow::relivenum),
			Corsairs::Util::MakeColumn("createdate", &AmphitheaterTeamRow::createdate, Corsairs::Util::Nullable | Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("updatetime", &AmphitheaterTeamRow::updatetime, Corsairs::Util::Timestamp),
		}) {}
};

class TableBoat : public Corsairs::Util::OdbcTable<BoatRow> {
public:
	explicit TableBoat(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "boat", {
			Corsairs::Util::MakeColumn("boat_id",         &BoatRow::boat_id, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("boat_berth",      &BoatRow::boat_berth),
			Corsairs::Util::MakeColumn("boat_name",       &BoatRow::boat_name),
			Corsairs::Util::MakeColumn("boat_boatid",     &BoatRow::boat_boatid),
			Corsairs::Util::MakeColumn("boat_header",     &BoatRow::boat_header),
			Corsairs::Util::MakeColumn("boat_body",       &BoatRow::boat_body),
			Corsairs::Util::MakeColumn("boat_engine",     &BoatRow::boat_engine),
			Corsairs::Util::MakeColumn("boat_cannon",     &BoatRow::boat_cannon),
			Corsairs::Util::MakeColumn("boat_equipment",  &BoatRow::boat_equipment),
			Corsairs::Util::MakeColumn("boat_bagsize",    &BoatRow::boat_bagsize),
			Corsairs::Util::MakeColumn("boat_bag",        &BoatRow::boat_bag),
			Corsairs::Util::MakeColumn("boat_diecount",   &BoatRow::boat_diecount),
			Corsairs::Util::MakeColumn("boat_isdead",     &BoatRow::boat_isdead),
			Corsairs::Util::MakeColumn("cur_endure",      &BoatRow::cur_endure),
			Corsairs::Util::MakeColumn("mx_endure",       &BoatRow::mx_endure),
			Corsairs::Util::MakeColumn("cur_supply",      &BoatRow::cur_supply),
			Corsairs::Util::MakeColumn("mx_supply",       &BoatRow::mx_supply),
			Corsairs::Util::MakeColumn("skill_state",     &BoatRow::skill_state),
			Corsairs::Util::MakeColumn("boat_ownerid",    &BoatRow::boat_ownerid),
			Corsairs::Util::MakeColumn("boat_createtime", &BoatRow::boat_createtime),
			Corsairs::Util::MakeColumn("boat_isdeleted",  &BoatRow::boat_isdeleted),
			Corsairs::Util::MakeColumn("map",             &BoatRow::map),
			Corsairs::Util::MakeColumn("map_x",           &BoatRow::map_x),
			Corsairs::Util::MakeColumn("map_y",           &BoatRow::map_y),
			Corsairs::Util::MakeColumn("angle",           &BoatRow::angle),
			Corsairs::Util::MakeColumn("degree",          &BoatRow::degree),
			Corsairs::Util::MakeColumn("exp",             &BoatRow::exp),
			Corsairs::Util::MakeColumn("version",         &BoatRow::version),
		}) {}
};

class TableCharacter : public Corsairs::Util::OdbcTable<CharacterRow> {
public:
	explicit TableCharacter(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "character", {
			Corsairs::Util::MakeColumn("atorID",           &CharacterRow::atorID, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("atorNome",         &CharacterRow::atorNome),
			Corsairs::Util::MakeColumn("motto",            &CharacterRow::motto),
			Corsairs::Util::MakeColumn("icon",             &CharacterRow::icon),
			Corsairs::Util::MakeColumn("version",          &CharacterRow::version),
			Corsairs::Util::MakeColumn("pk_ctrl",          &CharacterRow::pk_ctrl),
			Corsairs::Util::MakeColumn("endeMem",          &CharacterRow::endeMem),
			Corsairs::Util::MakeColumn("ato_id",           &CharacterRow::ato_id),
			Corsairs::Util::MakeColumn("guild_id",         &CharacterRow::guild_id),
			Corsairs::Util::MakeColumn("guild_stat",       &CharacterRow::guild_stat),
			Corsairs::Util::MakeColumn("guild_permission", &CharacterRow::guild_permission),
			Corsairs::Util::MakeColumn("job",              &CharacterRow::job),
			Corsairs::Util::MakeColumn("degree",           &CharacterRow::degree),
			Corsairs::Util::MakeColumn("exp",              &CharacterRow::exp),
			Corsairs::Util::MakeColumn("hp",               &CharacterRow::hp),
			Corsairs::Util::MakeColumn("sp",               &CharacterRow::sp),
			Corsairs::Util::MakeColumn("ap",               &CharacterRow::ap),
			Corsairs::Util::MakeColumn("tp",               &CharacterRow::tp),
			Corsairs::Util::MakeColumn("bomd",             &CharacterRow::bomd),
			Corsairs::Util::MakeColumn("str",              &CharacterRow::str),
			Corsairs::Util::MakeColumn("dex",              &CharacterRow::dex),
			Corsairs::Util::MakeColumn("agi",              &CharacterRow::agi),
			Corsairs::Util::MakeColumn("con",              &CharacterRow::con),
			Corsairs::Util::MakeColumn("sta",              &CharacterRow::sta),
			Corsairs::Util::MakeColumn("luk",              &CharacterRow::luk),
			Corsairs::Util::MakeColumn("sail_lv",          &CharacterRow::sail_lv),
			Corsairs::Util::MakeColumn("sail_exp",         &CharacterRow::sail_exp),
			Corsairs::Util::MakeColumn("sail_left_exp",    &CharacterRow::sail_left_exp),
			Corsairs::Util::MakeColumn("live_lv",          &CharacterRow::live_lv),
			Corsairs::Util::MakeColumn("live_exp",         &CharacterRow::live_exp),
			Corsairs::Util::MakeColumn("map",              &CharacterRow::map),
			Corsairs::Util::MakeColumn("map_x",            &CharacterRow::map_x),
			Corsairs::Util::MakeColumn("map_y",            &CharacterRow::map_y),
			Corsairs::Util::MakeColumn("radius",           &CharacterRow::radius),
			Corsairs::Util::MakeColumn("angle",            &CharacterRow::angle),
			Corsairs::Util::MakeColumn("olhe",             &CharacterRow::olhe),
			Corsairs::Util::MakeColumn("kb_capacity",      &CharacterRow::kb_capacity),
			Corsairs::Util::MakeColumn("kitbag",           &CharacterRow::kitbag),
			Corsairs::Util::MakeColumn("skillbag",         &CharacterRow::skillbag),
			Corsairs::Util::MakeColumn("shortcut",         &CharacterRow::shortcut),
			Corsairs::Util::MakeColumn("mission",          &CharacterRow::mission),
			Corsairs::Util::MakeColumn("misrecord",        &CharacterRow::misrecord),
			Corsairs::Util::MakeColumn("mistrigger",       &CharacterRow::mistrigger),
			Corsairs::Util::MakeColumn("miscount",         &CharacterRow::miscount),
			Corsairs::Util::MakeColumn("birth",            &CharacterRow::birth),
			Corsairs::Util::MakeColumn("login_cha",        &CharacterRow::login_cha),
			Corsairs::Util::MakeColumn("live_tp",          &CharacterRow::live_tp),
			Corsairs::Util::MakeColumn("delflag",          &CharacterRow::delflag),
			Corsairs::Util::MakeColumn("operdate",         &CharacterRow::operdate, Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("deldate",          &CharacterRow::deldate, Corsairs::Util::Nullable | Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("main_map",         &CharacterRow::main_map),
			Corsairs::Util::MakeColumn("skill_state",      &CharacterRow::skill_state),
			Corsairs::Util::MakeColumn("bank",             &CharacterRow::bank),
			Corsairs::Util::MakeColumn("estop",            &CharacterRow::estop, Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("estoptime",        &CharacterRow::estoptime),
			Corsairs::Util::MakeColumn("kb_locked",        &CharacterRow::kb_locked),
			Corsairs::Util::MakeColumn("kitbag_tmp",       &CharacterRow::kitbag_tmp),
			Corsairs::Util::MakeColumn("credit",           &CharacterRow::credit),
			Corsairs::Util::MakeColumn("store_item",       &CharacterRow::store_item),
			Corsairs::Util::MakeColumn("extend",           &CharacterRow::extend),
			Corsairs::Util::MakeColumn("chatColour",       &CharacterRow::chatColour),
			Corsairs::Util::MakeColumn("IMP",              &CharacterRow::IMP),
		}) {}
};

class TableCharacterLog : public Corsairs::Util::OdbcTable<CharacterLogRow> {
public:
	explicit TableCharacterLog(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "character_log", {
			Corsairs::Util::MakeColumn("atorID",   &CharacterLogRow::atorID),
			Corsairs::Util::MakeColumn("atorNome", &CharacterLogRow::atorNome),
			Corsairs::Util::MakeColumn("ato_id",   &CharacterLogRow::ato_id),
			Corsairs::Util::MakeColumn("guild_id", &CharacterLogRow::guild_id),
			Corsairs::Util::MakeColumn("job",      &CharacterLogRow::job),
			Corsairs::Util::MakeColumn("degree",   &CharacterLogRow::degree),
			Corsairs::Util::MakeColumn("exp",      &CharacterLogRow::exp),
			Corsairs::Util::MakeColumn("hp",       &CharacterLogRow::hp),
			Corsairs::Util::MakeColumn("sp",       &CharacterLogRow::sp),
			Corsairs::Util::MakeColumn("ap",       &CharacterLogRow::ap),
			Corsairs::Util::MakeColumn("tp",       &CharacterLogRow::tp),
			Corsairs::Util::MakeColumn("bomd",     &CharacterLogRow::bomd),
			Corsairs::Util::MakeColumn("str",      &CharacterLogRow::str),
			Corsairs::Util::MakeColumn("dex",      &CharacterLogRow::dex),
			Corsairs::Util::MakeColumn("agi",      &CharacterLogRow::agi),
			Corsairs::Util::MakeColumn("con",      &CharacterLogRow::con),
			Corsairs::Util::MakeColumn("sta",      &CharacterLogRow::sta),
			Corsairs::Util::MakeColumn("luk",      &CharacterLogRow::luk),
			Corsairs::Util::MakeColumn("map",      &CharacterLogRow::map),
			Corsairs::Util::MakeColumn("map_x",    &CharacterLogRow::map_x),
			Corsairs::Util::MakeColumn("map_y",    &CharacterLogRow::map_y),
			Corsairs::Util::MakeColumn("radius",   &CharacterLogRow::radius),
			Corsairs::Util::MakeColumn("olhe",     &CharacterLogRow::olhe),
			Corsairs::Util::MakeColumn("del_date", &CharacterLogRow::del_date, Corsairs::Util::Timestamp),
		}) {}
};

class TableFriends : public Corsairs::Util::OdbcTable<FriendsRow> {
public:
	explicit TableFriends(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "friends", {
			Corsairs::Util::MakeColumn("cha_id1",    &FriendsRow::cha_id1),
			Corsairs::Util::MakeColumn("cha_id2",    &FriendsRow::cha_id2),
			Corsairs::Util::MakeColumn("relation",   &FriendsRow::relation),
			Corsairs::Util::MakeColumn("createtime", &FriendsRow::createtime, Corsairs::Util::Nullable | Corsairs::Util::Timestamp),
		}) {}
};

class TableGuildTyped : public Corsairs::Util::OdbcTable<GuildRow> {
public:
	explicit TableGuildTyped(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "guild", {
			Corsairs::Util::MakeColumn("guild_id",     &GuildRow::guild_id, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("guild_name",   &GuildRow::guild_name),
			Corsairs::Util::MakeColumn("motto",        &GuildRow::motto),
			Corsairs::Util::MakeColumn("passwd",       &GuildRow::passwd),
			Corsairs::Util::MakeColumn("leader_id",    &GuildRow::leader_id),
			Corsairs::Util::MakeColumn("exp",          &GuildRow::exp),
			Corsairs::Util::MakeColumn("gold",         &GuildRow::gold),
			Corsairs::Util::MakeColumn("bank",         &GuildRow::bank),
			Corsairs::Util::MakeColumn("level",        &GuildRow::level),
			Corsairs::Util::MakeColumn("member_total", &GuildRow::member_total),
			Corsairs::Util::MakeColumn("try_total",    &GuildRow::try_total),
			Corsairs::Util::MakeColumn("disband_date", &GuildRow::disband_date, Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("challlevel",   &GuildRow::challlevel),
			Corsairs::Util::MakeColumn("challid",      &GuildRow::challid),
			Corsairs::Util::MakeColumn("challmoney",   &GuildRow::challmoney),
			Corsairs::Util::MakeColumn("challstart",   &GuildRow::challstart),
		}) {}
};

class TableLotterySetting : public Corsairs::Util::OdbcTable<LotterySettingRow> {
public:
	explicit TableLotterySetting(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "LotterySetting", {
			Corsairs::Util::MakeColumn("section",    &LotterySettingRow::section),
			Corsairs::Util::MakeColumn("issue",      &LotterySettingRow::issue),
			Corsairs::Util::MakeColumn("state",      &LotterySettingRow::state),
			Corsairs::Util::MakeColumn("createdate", &LotterySettingRow::createdate, Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("updatetime", &LotterySettingRow::updatetime, Corsairs::Util::Nullable | Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("itemno",     &LotterySettingRow::itemno),
		}) {}
};


class TableMaster : public Corsairs::Util::OdbcTable<MasterRow> {
public:
	explicit TableMaster(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "master", {
			Corsairs::Util::MakeColumn("cha_id1",  &MasterRow::cha_id1),
			Corsairs::Util::MakeColumn("cha_id2",  &MasterRow::cha_id2),
			Corsairs::Util::MakeColumn("finish",   &MasterRow::finish),
			Corsairs::Util::MakeColumn("relation", &MasterRow::relation),
		}) {}
};

class TableParam : public Corsairs::Util::OdbcTable<ParamRow> {
public:
	explicit TableParam(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "param", {
			Corsairs::Util::MakeColumn("id",      &ParamRow::id, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("param1",  &ParamRow::param1),
			Corsairs::Util::MakeColumn("param2",  &ParamRow::param2),
			Corsairs::Util::MakeColumn("param3",  &ParamRow::param3),
			Corsairs::Util::MakeColumn("param4",  &ParamRow::param4),
			Corsairs::Util::MakeColumn("param5",  &ParamRow::param5),
			Corsairs::Util::MakeColumn("param6",  &ParamRow::param6),
			Corsairs::Util::MakeColumn("param7",  &ParamRow::param7),
			Corsairs::Util::MakeColumn("param8",  &ParamRow::param8),
			Corsairs::Util::MakeColumn("param9",  &ParamRow::param9),
			Corsairs::Util::MakeColumn("param10", &ParamRow::param10),
		}) {}
};

class TablePersonAvatar : public Corsairs::Util::OdbcTable<PersonAvatarRow> {
public:
	explicit TablePersonAvatar(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "personavatar", {
			Corsairs::Util::MakeColumn("atorID", &PersonAvatarRow::atorID, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("avatar", &PersonAvatarRow::avatar),
		}) {}
};

class TablePersonInfo : public Corsairs::Util::OdbcTable<PersonInfoRow> {
public:
	explicit TablePersonInfo(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "personinfo", {
			Corsairs::Util::MakeColumn("atorID",        &PersonInfoRow::atorID, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("motto",         &PersonInfoRow::motto),
			Corsairs::Util::MakeColumn("showmotto",     &PersonInfoRow::showmotto),
			Corsairs::Util::MakeColumn("sex",           &PersonInfoRow::sex),
			Corsairs::Util::MakeColumn("age",           &PersonInfoRow::age),
			Corsairs::Util::MakeColumn("name",          &PersonInfoRow::name),
			Corsairs::Util::MakeColumn("animal_zodiac", &PersonInfoRow::animal_zodiac),
			Corsairs::Util::MakeColumn("blood_type",    &PersonInfoRow::blood_type),
			Corsairs::Util::MakeColumn("birthday",      &PersonInfoRow::birthday),
			Corsairs::Util::MakeColumn("state",         &PersonInfoRow::state),
			Corsairs::Util::MakeColumn("city",          &PersonInfoRow::city),
			Corsairs::Util::MakeColumn("constellation", &PersonInfoRow::constellation),
			Corsairs::Util::MakeColumn("career",        &PersonInfoRow::career),
			Corsairs::Util::MakeColumn("avatarsize",    &PersonInfoRow::avatarsize),
			Corsairs::Util::MakeColumn("prevent",       &PersonInfoRow::prevent),
			Corsairs::Util::MakeColumn("support",       &PersonInfoRow::support),
			Corsairs::Util::MakeColumn("oppose",        &PersonInfoRow::oppose),
		}) {}
};

class TableProperty : public Corsairs::Util::OdbcTable<PropertyRow> {
public:
	explicit TableProperty(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "property", {
			Corsairs::Util::MakeColumn("id",      &PropertyRow::id, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("atorID",  &PropertyRow::atorID),
			Corsairs::Util::MakeColumn("context", &PropertyRow::context),
			Corsairs::Util::MakeColumn("sum",     &PropertyRow::sum),
			Corsairs::Util::MakeColumn("time",    &PropertyRow::time, Corsairs::Util::Timestamp),
		}) {}
};

class TableResourceTyped : public Corsairs::Util::OdbcTable<ResourceRow> {
public:
	explicit TableResourceTyped(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "Resource", {
			Corsairs::Util::MakeColumn("id",      &ResourceRow::id, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("atorID",  &ResourceRow::atorID),
			Corsairs::Util::MakeColumn("type_id", &ResourceRow::type_id),
			Corsairs::Util::MakeColumn("content", &ResourceRow::content),
		}) {}
};

class TableStatLog : public Corsairs::Util::OdbcTable<StatLogRow> {
public:
	explicit TableStatLog(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "stat_log", {
			Corsairs::Util::MakeColumn("track_date",  &StatLogRow::track_date, Corsairs::Util::PrimaryKey | Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("login_num",   &StatLogRow::login_num),
			Corsairs::Util::MakeColumn("play_num",    &StatLogRow::play_num),
			Corsairs::Util::MakeColumn("wgplay_num",  &StatLogRow::wgplay_num),
		}) {}
};

class TableStatDegree : public Corsairs::Util::OdbcTable<StatDegreeRow> {
public:
	explicit TableStatDegree(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "StatDegree", {
			Corsairs::Util::MakeColumn("statDate",       &StatDegreeRow::statDate, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("degree",         &StatDegreeRow::degree, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("characterCount", &StatDegreeRow::characterCount),
		}) {}
};

class TableStatGender : public Corsairs::Util::OdbcTable<StatGenderRow> {
public:
	explicit TableStatGender(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "StatGender", {
			Corsairs::Util::MakeColumn("statDate",    &StatGenderRow::statDate, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("gender",      &StatGenderRow::gender, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("genderCount", &StatGenderRow::genderCount),
		}) {}
};

class TableStatJob : public Corsairs::Util::OdbcTable<StatJobRow> {
public:
	explicit TableStatJob(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "StatJob", {
			Corsairs::Util::MakeColumn("statDate",       &StatJobRow::statDate, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("job",            &StatJobRow::job, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("characterCount", &StatJobRow::characterCount),
		}) {}
};

class TableStatLogin : public Corsairs::Util::OdbcTable<StatLoginRow> {
public:
	explicit TableStatLogin(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "StatLogin", {
			Corsairs::Util::MakeColumn("statDate",   &StatLoginRow::statDate, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("loginCount", &StatLoginRow::loginCount),
		}) {}
};

class TableStatMap : public Corsairs::Util::OdbcTable<StatMapRow> {
public:
	explicit TableStatMap(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "StatMap", {
			Corsairs::Util::MakeColumn("statDate",  &StatMapRow::statDate, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("map",       &StatMapRow::map, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("playCount", &StatMapRow::playCount),
		}) {}
};

class TableTicket : public Corsairs::Util::OdbcTable<TicketRow> {
public:
	explicit TableTicket(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "Ticket", {
			Corsairs::Util::MakeColumn("id",      &TicketRow::id, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("atorID",  &TicketRow::atorID),
			Corsairs::Util::MakeColumn("issue",   &TicketRow::issue),
			Corsairs::Util::MakeColumn("itemno",  &TicketRow::itemno),
			Corsairs::Util::MakeColumn("real",    &TicketRow::real),
			Corsairs::Util::MakeColumn("buydate", &TicketRow::buydate, Corsairs::Util::Timestamp),
		}) {}
};

class TableTradeLog : public Corsairs::Util::OdbcTable<TradeLogRow> {
public:
	explicit TableTradeLog(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "Trade_Log", {
			Corsairs::Util::MakeColumn("ID",          &TradeLogRow::ID, Corsairs::Util::PrimaryKey),
			Corsairs::Util::MakeColumn("ExecuteTime", &TradeLogRow::ExecuteTime),
			Corsairs::Util::MakeColumn("GameServer",  &TradeLogRow::GameServer),
			Corsairs::Util::MakeColumn("Action",      &TradeLogRow::Action),
			Corsairs::Util::MakeColumn("From",        &TradeLogRow::From),
			Corsairs::Util::MakeColumn("To",          &TradeLogRow::To),
			Corsairs::Util::MakeColumn("Memo",        &TradeLogRow::Memo),
		}) {}
};

class TableWeekReport : public Corsairs::Util::OdbcTable<WeekReportRow> {
public:
	explicit TableWeekReport(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "weekreport", {
			Corsairs::Util::MakeColumn("ato_nome",   &WeekReportRow::ato_nome),
			Corsairs::Util::MakeColumn("atorNome",   &WeekReportRow::atorNome),
			Corsairs::Util::MakeColumn("degree",     &WeekReportRow::degree),
			Corsairs::Util::MakeColumn("ip",         &WeekReportRow::ip),
			Corsairs::Util::MakeColumn("createdate", &WeekReportRow::createdate, Corsairs::Util::Nullable | Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("logouttime", &WeekReportRow::logouttime, Corsairs::Util::Nullable | Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("playtime",   &WeekReportRow::playtime),
			Corsairs::Util::MakeColumn("Guild_Name", &WeekReportRow::Guild_Name),
		}) {}
};

class TableWinTicket : public Corsairs::Util::OdbcTable<WinTicketRow> {
public:
	explicit TableWinTicket(Corsairs::Util::OdbcDatabase& db)
		: OdbcTable(db, "WinTicket", {
			Corsairs::Util::MakeColumn("issue",      &WinTicketRow::issue),
			Corsairs::Util::MakeColumn("itemno",     &WinTicketRow::itemno),
			Corsairs::Util::MakeColumn("grade",      &WinTicketRow::grade),
			Corsairs::Util::MakeColumn("createdate", &WinTicketRow::createdate, Corsairs::Util::Timestamp),
			Corsairs::Util::MakeColumn("num",        &WinTicketRow::num),
		}) {}
};

// ============================================================================
// Legacy table classes
// ============================================================================

class PlayerStorage {
	Corsairs::Util::OdbcDatabase& _db;
	TableCharacter& _characters;

public:
	PlayerStorage(Corsairs::Util::OdbcDatabase& db, TableCharacter& characters)
		: _db(db), _characters(characters) {
	}

	bool ShowExpRank(CCharacter& pCha, std::int32_t count);
	bool Init(void);
	bool ReadAllData(CPlayer& player, std::uint32_t atorID);
	// bForceWithPos=true — сохранять map/main_map/map_x/map_y даже если
	// pCCtrlCha->GetSubMap() уже nullptr. Нужно для offline-save в
	// CGameApp::ReleaseGamePlayer, где мы снимаем персонажа с карты
	// (GoOut) ДО сохранения, чтобы избежать dangling в eyeshot.
	bool SaveAllData(CPlayer& pPlayer, char chSaveType, bool bForceWithPos = false); //
	bool SavePos(CPlayer& pPlayer); //
	bool SaveMoney(CPlayer& pPlayer);
	bool SaveKBagDBID(CPlayer& pPlayer);
	bool SaveKBagTmpDBID(CPlayer& pPlayer); // ID
	bool SaveKBState(CPlayer& pPlayer); //
	bool SaveBankDBID(CPlayer& pPlayer);
	bool SaveTableVer(std::uint32_t atorID); //
	bool SaveMissionData(CPlayer& pPlayer, std::uint32_t atorID); //
	bool VerifyName(const std::string& pszName); //
	std::string GetName(std::int32_t cha_id);

	bool AddCreditByDBID(std::uint32_t atorID, std::int32_t lCredit);
	bool IsChaOnline(std::uint32_t atorID, bool& bOnline);
	std::int32_t GetChaAddr(std::uint32_t atorID);
	bool SetChaAddr(std::uint32_t atorID, std::int32_t addr);

	bool SetGuildPermission(std::int32_t atorID, std::uint32_t perm, std::int32_t guild_id);


	bool SaveStoreItemID(std::uint32_t atorID, std::int32_t lStoreItemID);
	bool AddMoney(std::uint32_t atorID, std::int32_t money);

	bool SaveDaily(CPlayer& pPlayer);
};

enum ResDBTypeID {
	enumRESDB_TYPE_LOOK, //
	enumRESDB_TYPE_KITBAG, //
	enumRESDB_TYPE_BANK, //
	enumRESDB_TYPE_KITBAGTMP, //
};

// Add by lark.li 20080521 begin
enum IssueState {
	enumCURRENT = 0, //
	enumPASTDUE = 1, //
	enumDISUSE = 2, //
};

struct AmphitheaterSetting {
	enum AmphitheaterSateSetting {
		enumCURRENT = 0,
	};
};

//Add by sunny.sun 20080725
struct AmphitheaterTeam {
	enum AmphitheaterSateTeam {
		enumNotUse = 0, //
		enumUse = 1, //
		enumPromotion = 2, //
		enumRelive = 3, //
		enumOut = 4, //
	};
};

// Resource — kitbag/bank хранилище (Corsairs::Util::OdbcDatabase)
class CTableResource {
public:
	explicit CTableResource(Corsairs::Util::OdbcDatabase& db) : _db(db) {
	}

	bool Create(std::int32_t& lDBID, std::int32_t lChaId, std::int32_t lTypeId);
	bool ReadKitbagData(CCharacter& pCCha);
	bool SaveKitbagData(CCharacter& pCCha);
	bool ReadKitbagTmpData(CCharacter& pCCha);
	bool SaveKitbagTmpData(CCharacter& pCCha);
	bool ReadBankData(CPlayer& pCPly, std::int8_t chBankNO = -1);
	bool SaveBankData(CPlayer& pCPly, std::int8_t chBankNO = -1);

private:
	Corsairs::Util::OdbcDatabase& _db;
};

// Fog-of-war: per-(player, map) хранение в таблице player_map_masks.
// Заменил легаси CTableMapMask с фиксированными колонками content1..5 (см.
// databases/migrate_player_map_masks.sql).
class CTableMapMask {
public:
	explicit CTableMapMask(Corsairs::Util::OdbcDatabase& db) : _db(db) {
	}

	// Загружает все маски игрока (одним SELECT) и пушит их в pCPly через
	// SetMaskMapName + SetMapMaskBase64. Возвращает true при отсутствии ODBC-ошибок.
	bool ReadAllMaps(CPlayer& pCPly);

	// UPSERT'ит одну маску по (atorID, mapName). При bDirect=false складывает
	// готовый SQL в _saveQueue для отложенного выполнения через HandleSaveList.
	bool SaveMap(CPlayer& pCPly, const std::string& mapName,
	             const std::string& base64Data, bool bDirect = FALSE);

	void HandleSaveList();
	void SaveAll();

private:
	Corsairs::Util::OdbcDatabase& _db;
	std::list<std::string> _saveQueue;
};


class CTableBoat {
public:
	explicit CTableBoat(Corsairs::Util::OdbcDatabase& db) : _db(db) {
	}

	bool Create(std::uint32_t& dwBoatID, const BOAT_DATA& Data);
	bool GetBoat(CCharacter& Boat);
	bool SaveBoat(CCharacter& Boat, std::int8_t chSaveType);
	bool SaveBoatTempData(CCharacter& Boat, std::uint8_t byIsDeleted = 0);
	bool SaveBoatTempData(std::uint32_t dwBoatID, std::uint32_t dwOwnerID, std::uint8_t byIsDeleted = 0);
	bool SaveBoatDelTag(std::uint32_t dwBoatID, std::uint8_t byIsDeleted = 0);

	bool SaveAllData(CPlayer& pPlayer, std::int8_t chSaveType);
	bool ReadCabin(CCharacter& Boat);
	bool SaveCabin(CCharacter& Boat, std::int8_t chSaveType);
	bool SaveAllCabin(CPlayer& pPlayer, std::int8_t chSaveType);

private:
	Corsairs::Util::OdbcDatabase& _db;
};

class CTableGuild {
	Corsairs::Util::OdbcDatabase& _db;

public:
	explicit CTableGuild(Corsairs::Util::OdbcDatabase& db) : _db(db) {
	}


	struct BankLog {
		short type;
		time_t time;
		std::int64_t parameter; // ItemID or Gold value
		short quantity; // 1-99 for items, 0 for gold;
		short userID; // chaID of the actor
	};

	//std::vector<BankLog> data;

	std::int32_t Create(CCharacter& pCha, const std::string& guildname, const std::string& passwd);
	bool ListAll(CCharacter& pCha, std::int8_t disband_days);
	void TryFor(CCharacter& pCha, std::uint32_t guildid);
	void TryForConfirm(CCharacter& pCha, std::uint32_t guildid);
	bool GetGuildBank(std::uint32_t guildid, CKitbag* bag);

	bool UpdateGuildBank(std::uint32_t guildid, CKitbag* bag);
	std::int32_t GetGuildLeaderID(std::uint32_t guildid);

	bool SetGuildLog(std::vector<BankLog> log, std::uint32_t guildid);
	std::vector<BankLog> GetGuildLog(std::uint32_t guildid);


	bool UpdateGuildBankGold(std::int32_t guildID, std::int32_t money);
	std::int64_t GetGuildBankGold(std::uint32_t guildid);

	bool GetGuildInfo(CCharacter& pCha, std::uint32_t guildid);
	bool ListTryPlayer(CCharacter& pCha, char disband_days);
	bool Approve(CCharacter& pCha, std::uint32_t chaid);
	bool Reject(CCharacter& pCha, std::uint32_t chaid);
	bool Kick(CCharacter& pCha, std::uint32_t chaid);
	bool Leave(CCharacter& pCha);
	bool Disband(CCharacter& pCha, const std::string& passwd);
	bool Motto(CCharacter& pCha, const std::string& motto);
	bool GetGuildName(std::int32_t lGuildID, std::string& strGuildName);

	//
	bool Challenge(CCharacter& pCha, std::uint8_t byLevel, std::uint32_t dwMoney);
	bool Leizhu(CCharacter& pCha, std::uint8_t byLevel, std::uint32_t dwMoney);
	void ListChallenge(CCharacter& pCha);
	bool GetChallInfo(std::uint8_t byLevel, std::uint32_t& dwGuildID1, std::uint32_t& dwGuildID2, std::uint32_t& dwMoney);
	bool StartChall(std::uint8_t byLevel);
	bool HasCall(std::uint8_t byLevel);
	void EndChall(std::uint32_t dwGuild1, std::uint32_t dwGuild2, bool bChall);
	void ChallMoney(std::uint8_t byLevel, bool bChall, std::uint32_t dwGuildID, std::uint32_t dwChallID, std::uint32_t dwMoney);
	bool ChallWin(bool bUpdate, std::uint8_t byLevel, std::uint32_t dwWinGuildID, std::uint32_t dwFailerGuildID);
	bool HasGuildLevel(CCharacter& pChar, std::uint8_t byLevel);
};

class CGameDB {
public:
	CGameDB();
	~CGameDB();

	bool Init();

	Corsairs::Util::OdbcTransaction BeginTransaction();

	// Совместимость со старым кодом
	bool BeginTran();
	bool RollBack();
	bool CommitTran();

	bool ReadPlayer(CPlayer& pPlayer, std::uint32_t atorID);
	// bForceWithPos — см. PlayerStorage::SaveAllData.
	bool SavePlayer(CPlayer& pPlayer, std::int8_t chSaveType, bool bForceWithPos = false);

	bool SavePlayerKitbag(CPlayer& pPlayer, std::int8_t chSaveType = enumSAVE_TYPE_TRADE);
	bool SaveChaAssets(CCharacter& pCCha);

	// Лотерея — LotterySetting
	bool GetWinItemno(std::int32_t issue, std::string& itemno);
	bool GetLotteryIssue(std::int32_t& issue);
	bool AddIssue(std::int32_t issue);
	bool DisuseIssue(std::int32_t issue, std::int32_t state);

	// Лотерея — Ticket
	bool LotteryIsExsit(std::int32_t issue, const std::string& itemno);
	bool AddLotteryTicket(CCharacter& pCCha, std::int32_t issue, char itemno[6][2]);
	bool CalWinTicket(std::int32_t issue, std::int32_t max, std::string& itemno);

	// Амфитеатр
	bool IsValidAmphitheaterTeam(std::int32_t teamID, std::int32_t captainID, std::int32_t member1, std::int32_t member2);
	bool IsMasterRelation(std::int32_t masterID, std::int32_t prenticeID);

	// === AmphitheaterSetting ===
	bool GetAmphitheaterSeasonAndRound(std::int32_t& season, std::int32_t& round);
	bool AddAmphitheaterSeason(std::int32_t season);
	bool DisuseAmphitheaterSeason(std::int32_t season, std::int32_t state, const std::string& winner);
	bool UpdateAmphitheaterRound(std::int32_t season, std::int32_t round);

	// === AmphitheaterTeam ===
	bool GetAmphitheaterTeamCount(std::int32_t& count);
	bool GetAmphitheaterNoUseTeamID(std::int32_t& teamID);
	bool AmphitheaterTeamSignUP(std::int32_t& teamID, std::int32_t captain, std::int32_t member1, std::int32_t member2);
	bool AmphitheaterTeamCancel(std::int32_t teamID);
	bool IsAmphitheaterLogin(std::int32_t pActorID);
	bool IsMapFull(std::int32_t MapID, std::int32_t& PActorIDNum);
	bool UpdateMapNum(std::int32_t Teamid, std::int32_t Mapid, std::int32_t MapFlag);
	bool GetMapFlag(std::int32_t Teamid, std::int32_t& Mapflag);
	bool SetMaxBallotTeamRelive();
	bool SetMatchResult(std::int32_t Teamid1, std::int32_t Teamid2, std::int32_t Id1state, std::int32_t Id2state);
	bool GetCaptainByMapId(std::int32_t Mapid, std::string& Captainid1, std::string& Captainid2);
	bool UpdateMap(std::int32_t Mapid);
	bool UpdateMapAfterEnter(std::int32_t CaptainID, std::int32_t MapID);
	bool GetPromotionAndReliveTeam(std::vector<std::vector<std::string>>& dataPromotion,
								   std::vector<std::vector<std::string>>& dataRelive);
	bool UpdatReliveNum(std::int32_t ReID);
	bool UpdateAbsentTeamRelive();
	bool UpdateWinnum(std::int32_t teamid);
	bool GetUniqueMaxWinnum(std::int32_t& teamid);
	bool SetMatchnoState(std::int32_t teamid);
	bool UpdateState();
	bool CloseReliveByState(std::int32_t& statenum);
	bool CleanMapFlag(std::int32_t teamid1, std::int32_t teamid2);
	bool GetStateByTeamid(std::int32_t teamid, std::int32_t& state);

	bool UpdateIMP(CPlayer& ply);
	bool SaveGmLv(CPlayer& ply);

	std::string GetChaNameByID(std::int32_t cha_id);
	void ShowExpRank(CCharacter& pCha, std::int32_t top);
	bool SavePlayerPos(CPlayer& pPlayer);
	bool SavePlayerKBagDBID(CPlayer& pPlayer);
	bool SavePlayerKBagTmpDBID(CPlayer& pPlayer);

	bool CreatePlyBank(CPlayer& pCPly);
	bool SavePlyBank(CPlayer& pCPly, char chBankNO = -1);
	std::uint32_t GetPlayerMasterDBID(CPlayer& pPlayer);

	bool AddCreditByDBID(std::uint32_t atorID, std::int32_t lCredit);
	bool SaveStoreItemID(std::uint32_t atorID, std::int32_t lStoreItemID);
	bool AddMoney(std::uint32_t atorID, std::int32_t money);

	bool ReadKitbagTmpData(std::uint32_t res_id, std::string& strData);
	bool SaveKitbagTmpData(std::uint32_t res_id, const std::string& strData);

	bool IsChaOnline(std::uint32_t atorID, bool& bOnline);
	std::int32_t GetChaAddr(std::uint32_t atorID);
	std::int32_t SetGuildPermission(std::int32_t atorID, std::uint32_t perm, std::int32_t guild_id);
	std::int32_t SetChaAddr(std::uint32_t atorID, std::int32_t addr);

	bool SaveMissionData(CPlayer& pPlayer, std::uint32_t atorID);

	// Лодки
	bool Create(std::uint32_t& dwBoatID, const BOAT_DATA& Data);
	bool GetBoat(CCharacter& Boat);
	bool SaveBoat(CCharacter& Boat, char chSaveType);
	bool SaveBoatDelTag(std::uint32_t dwBoatID, std::uint8_t byIsDeleted = 0);
	bool SaveBoatTempData(CCharacter& Boat, std::uint8_t byIsDeleted = 0);
	bool SaveBoatTempData(std::uint32_t dwBoatID, std::uint32_t dwOwnerID, std::uint8_t byIsDeleted = 0);

	// Гильдии
	std::int32_t CreateGuild(CCharacter& pCha, const std::string& guildname, const std::string& passwd);
	std::int32_t GetGuildBank(std::uint32_t guildid, CKitbag* bag);
	std::int32_t UpdateGuildBank(std::uint32_t guildid, CKitbag* bag);
	bool SetGuildLog(std::vector<CTableGuild::BankLog> log, std::uint32_t guildid);
	std::vector<CTableGuild::BankLog> GetGuildLog(std::uint32_t guildid);
	std::int64_t GetGuildBankGold(std::uint32_t guildid);
	bool UpdateGuildBankGold(std::int32_t guildID, std::int32_t money);
	std::int32_t GetGuildLeaderID(std::uint32_t guildid);

	bool ListAllGuild(CCharacter& pCha, char disband_days = 1);
	void GuildTryFor(CCharacter& pCha, std::uint32_t guildid);
	void GuildTryForConfirm(CCharacter& pCha, std::uint32_t guildid);
	bool GuildListTryPlayer(CCharacter& pCha, char disband_days);
	bool GuildApprove(CCharacter& pCha, std::uint32_t chaid);
	bool GuildReject(CCharacter& pCha, std::uint32_t chaid);
	bool GuildKick(CCharacter& pCha, std::uint32_t chaid);
	bool GuildLeave(CCharacter& pCha);
	bool GuildDisband(CCharacter& pCha, const std::string& passwd);
	bool GuildMotto(CCharacter& pCha, const std::string& motto);

	CTableMapMask* GetMapMaskTable();

	bool GetGuildName(std::int32_t lGuildID, std::string& strGuildName);
	bool Challenge(CCharacter& pCha, std::uint8_t byLevel, std::uint32_t dwMoney);
	bool Leizhu(CCharacter& pCha, std::uint8_t byLevel, std::uint32_t dwMoney);
	void ListChallenge(CCharacter& pCha);
	bool StartChall(std::uint8_t byLevel);
	bool GetChall(std::uint8_t byLevel, std::uint32_t& dwGuildID1, std::uint32_t& dwGuildID2, std::uint32_t& dwMoney);
	void EndChall(std::uint32_t dwGuild1, std::uint32_t dwGuild2, bool bChall);
	bool HasChall(std::uint8_t byLevel);
	bool HasGuildLevel(CCharacter& pChar, std::uint8_t byLevel);

	// Логирование
	void ExecLogSQL(const std::string& pszSQL);
	void ExecTradeLogSQL(const std::string& gameServerName, const std::string& action,
						 const std::string& pszChaFrom, const std::string& pszChaTo, const std::string& pszTrade);

	bool m_bInitOK{false};

protected:
	// ODBC API — объявлен первым, т.к. все таблицы зависят от _db
	Corsairs::Util::OdbcDatabase _db{};

	// Legacy-таблицы (прямые члены)
	PlayerStorage _tab_cha;
	CTableResource _tab_res;
	CTableMapMask _tab_mmask;
	CTableGuild _tab_gld;
	CTableBoat _tab_boat;

	// Типизированные таблицы
	TableAccount _accounts;
	TableAmphitheaterSetting _amphiSettings;
	TableAmphitheaterTeam _amphiTeams;
	TableBoat _boats;
	TableCharacter _characters;
	TableCharacterLog _characterLogs;
	TableFriends _friends;
	TableGuildTyped _guilds;
	TableLotterySetting _lotterySettings;
	TableMaster _masters;
	TableParam _params;
	TablePersonAvatar _personAvatars;
	TablePersonInfo _personInfo;
	TableProperty _properties;
	TableResourceTyped _resources;
	TableStatLog _statLogs;
	TableStatDegree _statDegrees;
	TableStatGender _statGenders;
	TableStatJob _statJobs;
	TableStatLogin _statLogins;
	TableStatMap _statMaps;
	TableTicket _tickets;
	TableTradeLog _tradeLogs;
	TableWeekReport _weekReports;
	TableWinTicket _winTickets;
};


extern CGameDB game_db;
