//
// Created by yuhang on 2025/4/27.
//

#include "ecleaner.h"
#include "version.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string_view>

translate Tran;
const string data_path = "plugins/ecleaner";
const std::string config_path = "plugins/ecleaner/config.json";
bool next_clean = false;
shared_ptr<endstone::Task> auto_clean_task;

// config
bool auto_item_clean = true;
bool auto_entity_clean = true;
bool item_clean_whitelist = true;
vector<string> item_clean_list;
bool entity_clean_whitelist = false;
vector<string> entity_clean_list;
int clean_tps = 16;
int clean_time = 15;
int last_entity = 0;

namespace {
constexpr std::uint64_t kTpsCheckPeriod = 100;
constexpr std::uint64_t kAutoCleanWarningDelay = 600;

bool protect_named_entities = true;
bool protect_named_items = true;
bool protect_valuable_items = true;
vector<string> valuable_item_keywords;

const vector<string> item_clean_list_default = {"Shulker Box", "White Shulker Box", "Light Gray Shulker Box",
                                                "Gray Shulker Box",  "Black Shulker Box", "Brown Shulker Box",
                                                "Red Shulker Box",   "Orange Shulker Box", "Yellow Shulker Box",
                                                "Lime Shulker Box",  "Green Shulker Box", "Cyan Shulker Box",
                                                "Light Blue Shulker Box", "Blue Shulker Box", "Purple Shulker Box",
                                                "Magenta Shulker Box",    "Pink Shulker Box"};

const vector<string> entity_clean_list_default = {"minecraft:zombie_pigman", "minecraft:zombie", "minecraft:skeleton",
                                                  "minecraft:bogged", "minecraft:slime"};

const vector<string> valuable_item_keywords_default = {"diamond", "netherite", "shulker_box"};

json make_default_config()
{
    return {
        {"language", "zh_CN"},
        {"auto_item_clean", true},
        {"auto_entity_clean", true},
        {"item_clean_whitelist", true},
        {"item_clean_list", item_clean_list_default},
        {"entity_clean_whitelist", false},
        {"entity_clean_list", entity_clean_list_default},
        {"protect_named_entities", true},
        {"protect_named_items", true},
        {"protect_valuable_items", true},
        {"valuable_item_keywords", valuable_item_keywords_default},
        {"clean_tps", 16},
        {"clean_time", 15},
    };
}

void reset_config_defaults()
{
    auto_item_clean = true;
    auto_entity_clean = true;
    item_clean_whitelist = true;
    entity_clean_whitelist = false;
    item_clean_list = item_clean_list_default;
    entity_clean_list = entity_clean_list_default;
    protect_named_entities = true;
    protect_named_items = true;
    protect_valuable_items = true;
    valuable_item_keywords = valuable_item_keywords_default;
    clean_tps = 16;
    clean_time = 15;
}

std::string normalize_text(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

void normalize_valuable_item_keywords()
{
    for (auto &keyword : valuable_item_keywords) {
        keyword = normalize_text(keyword);
    }
}

bool has_name_tag(const endstone::Actor &actor)
{
    return !actor.getNameTag().empty();
}

bool is_named_item(const endstone::Actor &actor)
{
    if (has_name_tag(actor)) {
        return true;
    }

    const auto *item_actor = actor.asItem();
    if (!item_actor) {
        return false;
    }

    const auto stack = item_actor->getItemStack();
    if (!stack.hasItemMeta()) {
        return false;
    }

    const auto meta = stack.getItemMeta();
    return meta && meta->hasDisplayName() && !meta->getDisplayName().empty();
}

bool is_valuable_item(const endstone::Actor &actor)
{
    const auto *item_actor = actor.asItem();
    if (!item_actor) {
        return false;
    }

    const auto stack = item_actor->getItemStack();
    const auto item_id = normalize_text(static_cast<std::string>(stack.getType().getId()));

    return std::any_of(valuable_item_keywords.begin(), valuable_item_keywords.end(), [&](const std::string &keyword) {
        return !keyword.empty() && item_id.find(keyword) != std::string::npos;
    });
}

bool should_skip_item_clean(const endstone::Actor &actor)
{
    if (protect_named_items && is_named_item(actor)) {
        return true;
    }

    if (protect_valuable_items && is_valuable_item(actor)) {
        return true;
    }

    return false;
}

bool should_skip_entity_clean(const endstone::Actor &actor)
{
    return protect_named_entities && has_name_tag(actor);
}

std::pair<bool, std::string> load_config_state(ECleaner &plugin, const json &config)
{
    reset_config_defaults();
    std::string language = "zh_CN";

    if (config.contains("error")) {
        plugin.getLogger().error(Tran.getLocal("Config file error!Use default config"));
        normalize_valuable_item_keywords();
        return {false, language};
    }

    try {
        auto_item_clean = config.value("auto_item_clean", auto_item_clean);
        auto_entity_clean = config.value("auto_entity_clean", auto_entity_clean);
        item_clean_whitelist = config.value("item_clean_whitelist", item_clean_whitelist);
        entity_clean_whitelist = config.value("entity_clean_whitelist", entity_clean_whitelist);
        item_clean_list = config.value("item_clean_list", item_clean_list);
        entity_clean_list = config.value("entity_clean_list", entity_clean_list);
        protect_named_entities = config.value("protect_named_entities", protect_named_entities);
        protect_named_items = config.value("protect_named_items", protect_named_items);
        protect_valuable_items = config.value("protect_valuable_items", protect_valuable_items);
        valuable_item_keywords = config.value("valuable_item_keywords", valuable_item_keywords);
        clean_tps = config.value("clean_tps", clean_tps);
        clean_time = config.value("clean_time", clean_time);
        language = config.value("language", language);
    } catch (const std::exception &e) {
        plugin.getLogger().error(Tran.getLocal("Config file error!Use default config") + "," + e.what());
        reset_config_defaults();
        normalize_valuable_item_keywords();
        return {false, language};
    }

    normalize_valuable_item_keywords();
    return {true, language};
}

void reload_language_file(const std::string &language)
{
    language_file = language_path + language + ".json";
    Tran = translate(language_file);
    Tran.loadLanguage();
}

void restart_auto_clean_task(ECleaner &plugin)
{
    if (auto_clean_task) {
        auto_clean_task->cancel();
        auto_clean_task.reset();
    }

    if (clean_time >= 1) {
        const std::uint64_t clean_interval = static_cast<std::uint64_t>(clean_time) * 60 * 20;
        auto_clean_task = plugin.getServer().getScheduler().runTaskTimer(plugin, [&plugin]() { plugin.auto_clean(); }, 0,
                                                                         clean_interval);
    }
}

bool reload_runtime_config(ECleaner &plugin, endstone::CommandSender *sender = nullptr)
{
    const auto config = plugin.read_config();
    auto [loaded, language] = load_config_state(plugin, config);
    reload_language_file(language);
    restart_auto_clean_task(plugin);
    last_entity = static_cast<int>(plugin.getServer().getLevel()->getActors().size());

    if (sender) {
        if (loaded) {
            sender->sendMessage(Tran.getLocal("Reload completed."));
        } else {
            sender->sendErrorMessage(Tran.getLocal("Config file error!Use default config"));
        }
    }

    return loaded;
}

json load_config_for_write(ECleaner &plugin)
{
    const auto config = plugin.read_config();
    if (config.contains("error")) {
        return make_default_config();
    }
    return config;
}
}  // namespace

// 数据目录和配置文件检查
void ECleaner::datafile_check() const
{
    namespace fs = std::filesystem;
    auto default_config = make_default_config();

    if (!fs::exists(data_path)) {
        getLogger().info(Tran.getLocal("No data path,auto create"));
        fs::create_directories(data_path);
    }

    if (!fs::exists(config_path)) {
        if (std::ofstream file(config_path); file.is_open()) {
            file << default_config.dump(4);
            getLogger().info(Tran.getLocal("Config file created"));
        }
    } else {
        bool need_update = false;
        auto loaded_config = read_config();

        if (loaded_config.contains("error")) {
            loaded_config = default_config;
            need_update = true;
        }

        for (const auto &[key, value] : default_config.items()) {
            if (!loaded_config.contains(key)) {
                loaded_config[key] = value;
                getLogger().info(Tran.tr("Config '{}' has update with default config", key));
                need_update = true;
            }
        }

        if (need_update) {
            if (std::ofstream outfile(config_path); outfile.is_open()) {
                outfile << loaded_config.dump(4);
                getLogger().info(Tran.getLocal("Config file update over"));
            }
        }
    }

    if (!fs::exists(language_path)) {
        fs::create_directories(language_path);
    }
}

// 读取配置文件
[[nodiscard]] json ECleaner::read_config() const
{
    std::ifstream file(config_path);
    if (!file.is_open()) {
        return {{"error", "error"}};
    }

    try {
        json loaded_config;
        file >> loaded_config;
        return loaded_config;
    } catch (const json::parse_error &ex) {
        getLogger().error(ex.what());
        return {{"error", "error"}};
    }
}

// 清理掉落物
[[nodiscard]] int ECleaner::clean_item() const
{
    int total_clean_num = 0;

    for (const auto &one_actor : getServer().getLevel()->getActors()) {
        if (!one_actor->asItem()) {
            continue;
        }

        if (should_skip_item_clean(*one_actor)) {
            continue;
        }

        const bool matched = std::find(item_clean_list.begin(), item_clean_list.end(), one_actor->getName()) !=
                             item_clean_list.end();
        const bool should_clean = item_clean_whitelist ? !matched : matched;

        if (should_clean) {
            one_actor->remove();
            total_clean_num += 1;
        }
    }

    return total_clean_num;
}

// 清理实体
[[nodiscard]] int ECleaner::clean_entity() const
{
    int total_clean_num = 0;

    for (const auto &one_actor : getServer().getLevel()->getActors()) {
        if (one_actor->asItem()) {
            continue;
        }

        if (should_skip_entity_clean(*one_actor)) {
            continue;
        }

        const bool matched =
            std::find(entity_clean_list.begin(), entity_clean_list.end(), one_actor->getType()) != entity_clean_list.end();
        const bool should_clean = entity_clean_whitelist ? !matched : matched;

        if (should_clean) {
            one_actor->remove();
            total_clean_num += 1;
        }
    }

    return total_clean_num;
}

// 自动检查当前服务器状态是否可执行清理
void ECleaner::check_server_run_clean() const
{
    if (getServer().getOnlinePlayers().empty()) {
        return;
    }

    if (static_cast<int>(getServer().getAverageTicksPerSecond()) >= clean_tps && !next_clean) {
        return;
    }

    if (std::abs(static_cast<int>(getServer().getLevel()->getActors().size()) - last_entity) < 20 && !next_clean) {
        return;
    }

    if (!next_clean) {
        next_clean = true;
        if (endstone::CommandSenderWrapper command_sender_wrapper(getServer().getCommandSender());
            getServer().dispatchCommand(command_sender_wrapper, "playsound note.banjo @a")) {
        }
        getServer().broadcastMessage("§l§2 [ECleaner] §r" + endstone::ColorFormat::Yellow +
                                     Tran.getLocal("The server's current average TPS has fallen below the set value, and entity cleanup will begin soon."));
        return;
    }

    run_clean();
    next_clean = false;
}

// 执行清理
void ECleaner::run_clean() const
{
    int clean_item_num = 0;
    int clean_entity_num = 0;

    if (auto_item_clean) {
        clean_item_num = clean_item();
        getServer().broadcastMessage("§l§2 [ECleaner] §r" + endstone::ColorFormat::Yellow +
                                     Tran.getLocal("Number of dropped items cleaned up: ") + to_string(clean_item_num));
    }

    if (auto_entity_clean) {
        clean_entity_num = clean_entity();
        getServer().broadcastMessage("§l§2 [ECleaner] §r" + endstone::ColorFormat::Yellow +
                                     Tran.getLocal("Number of entities cleaned up: ") + to_string(clean_entity_num));
    }

    if (clean_entity_num == 0 && clean_item_num == 0) {
        getServer().broadcastMessage("§l§2 [ECleaner] §r" + endstone::ColorFormat::Yellow +
                                     Tran.getLocal("No entities were cleaned up."));
    }

    last_entity = static_cast<int>(getServer().getLevel()->getActors().size());
}

// 手动执行掉落物清理
void ECleaner::run_clean_item() const
{
    const int clean_item_num = clean_item();
    getServer().broadcastMessage("§l§2 [ECleaner] §r" + endstone::ColorFormat::Yellow +
                                 Tran.getLocal("Number of dropped items cleaned up: ") + to_string(clean_item_num));
}

// 手动执行实体清理
void ECleaner::run_clean_entity() const
{
    const int clean_entity_num = clean_entity();
    getServer().broadcastMessage("§l§2 [ECleaner] §r" + endstone::ColorFormat::Yellow +
                                 Tran.getLocal("Number of entities cleaned up: ") + to_string(clean_entity_num));
}

// 定期清理
void ECleaner::auto_clean()
{
    if (getServer().getOnlinePlayers().empty()) {
        return;
    }

    if (endstone::CommandSenderWrapper command_sender_wrapper(getServer().getCommandSender());
        getServer().dispatchCommand(command_sender_wrapper, "playsound note.banjo @a")) {
    }

    getServer().broadcastMessage("§l§2 [ECleaner] §r" + endstone::ColorFormat::Yellow +
                                 Tran.getLocal("There are 30 seconds remaining until the server entity cleanup begins."));
    getServer().getScheduler().runTaskLater(*this, [this]() { run_clean(); }, kAutoCleanWarningDelay);
}

void ECleaner::onLoad()
{
    getLogger().info("onLoad is called");
    language_file = language_path + getServer().getLanguage().getLocale() + ".json";
    Tran = translate(language_file);
    Tran.loadLanguage();
    datafile_check();
}

void ECleaner::onEnable()
{
    getLogger().info("onEnable is called");
    getLogger().info(endstone::ColorFormat::Yellow + Tran.getLocal("ECleaner has been enable,version: ") +
                     getServer().getPluginManager().getPlugin("ecleaner")->getDescription().getVersion());

    reload_runtime_config(*this);
    getServer().getScheduler().runTaskTimer(*this, [this]() { check_server_run_clean(); }, 0, kTpsCheckPeriod);
}

void ECleaner::onDisable()
{
    getLogger().info("onDisable is called");
}

bool ECleaner::onCommand(endstone::CommandSender &sender, const endstone::Command &command,
                         const std::vector<std::string> &args)
{
    if (command.getName() != "ecl") {
        return false;
    }

    if (args.empty()) {
        if (const auto player = sender.asPlayer()) {
            ecl_main_menu(*player);
        }
        return true;
    }

    if (args[0] == "clean") {
        if (args.size() >= 2 && args[1] == "entity") {
            run_clean_entity();
        } else if (args.size() >= 2 && args[1] == "item") {
            run_clean_item();
        } else {
            run_clean();
        }
        return true;
    }

    if (args[0] == "reload") {
        reload_runtime_config(*this, &sender);
        return true;
    }

    return true;
}

// ecl 菜单
void ECleaner::ecl_main_menu(endstone::Player &player)
{
    endstone::ModalForm menu;
    menu.setTitle(Tran.getLocal("ECL Config Menu"));

    endstone::Toggle auto_entity_clean_toggle;
    endstone::Toggle auto_item_clean_toggle;
    endstone::Toggle entity_whitelist_toggle;
    endstone::Toggle item_whitelist_toggle;
    endstone::Toggle protect_named_entities_toggle;
    endstone::Toggle protect_named_items_toggle;
    endstone::Toggle protect_valuable_items_toggle;
    endstone::Slider clean_time_slider;
    endstone::Slider clean_tps_slider;

    auto_entity_clean_toggle.setLabel(Tran.getLocal("Auto clean entity"));
    auto_entity_clean_toggle.setDefaultValue(auto_entity_clean);

    auto_item_clean_toggle.setLabel(Tran.getLocal("Auto clean item"));
    auto_item_clean_toggle.setDefaultValue(auto_item_clean);

    entity_whitelist_toggle.setLabel(Tran.getLocal("Entity whitelist mode"));
    entity_whitelist_toggle.setDefaultValue(entity_clean_whitelist);

    item_whitelist_toggle.setLabel(Tran.getLocal("Item whitelist mode"));
    item_whitelist_toggle.setDefaultValue(item_clean_whitelist);

    protect_named_entities_toggle.setLabel(Tran.getLocal("Protect named entities"));
    protect_named_entities_toggle.setDefaultValue(protect_named_entities);

    protect_named_items_toggle.setLabel(Tran.getLocal("Protect named dropped items"));
    protect_named_items_toggle.setDefaultValue(protect_named_items);

    protect_valuable_items_toggle.setLabel(Tran.getLocal("Protect valuable dropped items"));
    protect_valuable_items_toggle.setDefaultValue(protect_valuable_items);

    clean_time_slider.setLabel(Tran.getLocal("Scheduled cleanup interval time(min)"));
    clean_time_slider.setMin(0);
    clean_time_slider.setMax(60);
    clean_time_slider.setStep(1);
    clean_time_slider.setDefaultValue(static_cast<float>(clean_time));

    clean_tps_slider.setLabel(Tran.getLocal("Minimum TPS to trigger automatic cleanup"));
    clean_tps_slider.setMin(1);
    clean_tps_slider.setMax(20);
    clean_tps_slider.setStep(1);
    clean_tps_slider.setDefaultValue(static_cast<float>(clean_tps));

    menu.setControls({auto_entity_clean_toggle, auto_item_clean_toggle, entity_whitelist_toggle, item_whitelist_toggle,
                      protect_named_entities_toggle, protect_named_items_toggle, protect_valuable_items_toggle,
                      clean_time_slider, clean_tps_slider});

    menu.setOnSubmit([this](endstone::Player *player_sender, const string &response) {
        if (!player_sender || response.empty() || response == "null") {
            return;
        }

        json response_json;
        try {
            response_json = json::parse(response);
        } catch (const std::exception &) {
            return;
        }

        const bool auto_entity_clean_new = response_json[0].get<bool>();
        const bool auto_item_clean_new = response_json[1].get<bool>();
        const bool entity_clean_whitelist_new = response_json[2].get<bool>();
        const bool item_clean_whitelist_new = response_json[3].get<bool>();
        const bool protect_named_entities_new = response_json[4].get<bool>();
        const bool protect_named_items_new = response_json[5].get<bool>();
        const bool protect_valuable_items_new = response_json[6].get<bool>();
        const int clean_time_new = response_json[7].get<int>();
        const int clean_tps_new = response_json[8].get<int>();

        bool need_update = false;
        auto updated_config = load_config_for_write(*this);

        if (auto_entity_clean != auto_entity_clean_new) {
            auto_entity_clean = auto_entity_clean_new;
            updated_config["auto_entity_clean"] = auto_entity_clean;
            need_update = true;
        }
        if (auto_item_clean != auto_item_clean_new) {
            auto_item_clean = auto_item_clean_new;
            updated_config["auto_item_clean"] = auto_item_clean;
            need_update = true;
        }
        if (entity_clean_whitelist != entity_clean_whitelist_new) {
            entity_clean_whitelist = entity_clean_whitelist_new;
            updated_config["entity_clean_whitelist"] = entity_clean_whitelist;
            need_update = true;
        }
        if (item_clean_whitelist != item_clean_whitelist_new) {
            item_clean_whitelist = item_clean_whitelist_new;
            updated_config["item_clean_whitelist"] = item_clean_whitelist;
            need_update = true;
        }
        if (protect_named_entities != protect_named_entities_new) {
            protect_named_entities = protect_named_entities_new;
            updated_config["protect_named_entities"] = protect_named_entities;
            need_update = true;
        }
        if (protect_named_items != protect_named_items_new) {
            protect_named_items = protect_named_items_new;
            updated_config["protect_named_items"] = protect_named_items;
            need_update = true;
        }
        if (protect_valuable_items != protect_valuable_items_new) {
            protect_valuable_items = protect_valuable_items_new;
            updated_config["protect_valuable_items"] = protect_valuable_items;
            need_update = true;
        }
        if (clean_time != clean_time_new) {
            clean_time = clean_time_new;
            updated_config["clean_time"] = clean_time;
            restart_auto_clean_task(*this);
            need_update = true;
        }
        if (clean_tps != clean_tps_new) {
            clean_tps = clean_tps_new;
            updated_config["clean_tps"] = clean_tps;
            need_update = true;
        }

        if (need_update) {
            if (std::ofstream outfile(config_path); outfile.is_open()) {
                outfile << updated_config.dump(4);
                player_sender->sendMessage(Tran.getLocal("Config file update over"));
            }
        }
    });

    player.sendForm(menu);
}

// 插件信息
ENDSTONE_PLUGIN("ecleaner", ECLEANER_PLUGIN_VERSION, ECleaner)
{
    description = "a plugin for endstone to clean entity";

    command("ecl")
        .description("ECleaner")
        .usages("/ecl", "/ecl clean [entity|item]", "/ecl reload")
        .permissions("ecleaner.command.op");

    permission("ecleaner.command.op").description("ecl op command").default_(endstone::PermissionDefault::Operator);
}
