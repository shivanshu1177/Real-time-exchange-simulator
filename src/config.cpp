#include "rtes/config.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>

namespace rtes {

namespace {

// Find value string for a JSON key within a block of text.
// Returns empty string if not found.
std::string extract_string(const std::string& content, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = content.find(search);
    if (pos == std::string::npos) return {};
    pos = content.find(':', pos + search.size());
    if (pos == std::string::npos) return {};
    // Skip whitespace
    ++pos;
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' ||
                                    content[pos] == '\n' || content[pos] == '\r')) ++pos;
    if (pos >= content.size()) return {};
    if (content[pos] == '"') {
        auto end = content.find('"', pos + 1);
        if (end == std::string::npos) return {};
        return content.substr(pos + 1, end - pos - 1);
    }
    // non-string (number/bool): read until delimiter
    auto end = pos;
    while (end < content.size() && content[end] != ',' && content[end] != '\n' &&
           content[end] != '}' && content[end] != ']') ++end;
    // trim trailing whitespace
    while (end > pos && (content[end - 1] == ' ' || content[end - 1] == '\r')) --end;
    return content.substr(pos, end - pos);
}

uint64_t extract_uint64(const std::string& content, const std::string& key) {
    auto v = extract_string(content, key);
    if (v.empty()) return 0;
    try { return std::stoull(v); } catch (...) { return 0; }
}

uint16_t extract_uint16(const std::string& content, const std::string& key) {
    return static_cast<uint16_t>(extract_uint64(content, key));
}

uint32_t extract_uint32(const std::string& content, const std::string& key) {
    return static_cast<uint32_t>(extract_uint64(content, key));
}

double extract_double(const std::string& content, const std::string& key) {
    auto v = extract_string(content, key);
    if (v.empty()) return 0.0;
    try { return std::stod(v); } catch (...) { return 0.0; }
}

bool extract_bool(const std::string& content, const std::string& key) {
    auto v = extract_string(content, key);
    return v == "true";
}

// Extract all {...} blocks inside the "symbols" array
std::vector<std::string> extract_symbol_blocks(const std::string& content) {
    std::vector<std::string> blocks;
    auto arr_start = content.find("\"symbols\"");
    if (arr_start == std::string::npos) return blocks;
    arr_start = content.find('[', arr_start);
    if (arr_start == std::string::npos) return blocks;

    size_t pos = arr_start + 1;
    while (pos < content.size()) {
        auto obj_start = content.find('{', pos);
        if (obj_start == std::string::npos) break;
        // find matching closing brace
        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < content.size() && depth > 0) {
            if (content[obj_end] == '{') ++depth;
            else if (content[obj_end] == '}') --depth;
            ++obj_end;
        }
        if (depth == 0) {
            blocks.push_back(content.substr(obj_start, obj_end - obj_start));
        }
        pos = obj_end;
        // Stop if we hit the closing ] of the array
        auto next_bracket = content.find(']', obj_end > 0 ? obj_end - 1 : 0);
        auto next_brace   = content.find('{', obj_end);
        if (next_bracket != std::string::npos &&
            (next_brace == std::string::npos || next_bracket < next_brace)) break;
    }
    return blocks;
}

Config make_defaults() {
    Config c;
    c.exchange.name               = "RTES";
    c.exchange.tcp_port           = 8888;
    c.exchange.udp_multicast_group= "239.0.0.1";
    c.exchange.udp_port           = 9999;
    c.exchange.metrics_port       = 8080;

    c.symbols.push_back({"AAPL",  0.01, 1, 10.0});
    c.symbols.push_back({"MSFT",  0.01, 1, 10.0});
    c.symbols.push_back({"GOOGL", 0.01, 1, 10.0});

    c.risk.max_order_size          = 10000;
    c.risk.max_notional_per_client = 1000000.0;
    c.risk.max_orders_per_second   = 1000;
    c.risk.price_collar_enabled    = true;

    c.performance.order_pool_size  = 1000000;
    c.performance.queue_capacity   = 65536;
    c.performance.enable_cpu_pinning = false;
    c.performance.tcp_nodelay      = true;
    c.performance.udp_buffer_size  = 262144;

    c.logging.level                = "INFO";
    c.logging.rate_limit_ms        = 1000;
    c.logging.enable_structured    = true;

    c.persistence.enable_event_log     = false;
    c.persistence.snapshot_interval_ms = 60000;
    c.persistence.log_directory        = "./logs";
    return c;
}

} // anonymous namespace

std::unique_ptr<Config> Config::load_from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[Config] Cannot open " << path << " — using defaults\n";
        return std::make_unique<Config>(make_defaults());
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string content = ss.str();

    auto cfg = std::make_unique<Config>(make_defaults());

    // Exchange block
    cfg->exchange.name                = extract_string(content, "name");
    if (cfg->exchange.name.empty()) cfg->exchange.name = "RTES";
    auto tcp_port = extract_uint16(content, "tcp_port");
    if (tcp_port) cfg->exchange.tcp_port = tcp_port;
    auto udp_group = extract_string(content, "udp_multicast_group");
    if (!udp_group.empty()) cfg->exchange.udp_multicast_group = udp_group;
    auto udp_port = extract_uint16(content, "udp_port");
    if (udp_port) cfg->exchange.udp_port = udp_port;
    auto metrics_port = extract_uint16(content, "metrics_port");
    if (metrics_port) cfg->exchange.metrics_port = metrics_port;
    auto cod = extract_string(content, "cancel_on_disconnect");
    if (!cod.empty()) cfg->exchange.cancel_on_disconnect = (cod == "true");

    // Risk block
    auto max_order_size = extract_uint64(content, "max_order_size");
    if (max_order_size) cfg->risk.max_order_size = max_order_size;
    auto max_notional = extract_double(content, "max_notional_per_client");
    if (max_notional > 0) cfg->risk.max_notional_per_client = max_notional;
    auto max_ops = extract_uint32(content, "max_orders_per_second");
    if (max_ops) cfg->risk.max_orders_per_second = max_ops;
    cfg->risk.price_collar_enabled = extract_bool(content, "price_collar_enabled");

    // Performance block
    auto pool_size = extract_uint32(content, "order_pool_size");
    if (pool_size) cfg->performance.order_pool_size = pool_size;
    auto queue_cap = extract_uint32(content, "queue_capacity");
    if (queue_cap) cfg->performance.queue_capacity = queue_cap;
    cfg->performance.tcp_nodelay = extract_bool(content, "tcp_nodelay");
    auto udp_buf = extract_uint32(content, "udp_buffer_size");
    if (udp_buf) cfg->performance.udp_buffer_size = udp_buf;

    // Logging block
    auto log_level = extract_string(content, "level");
    if (!log_level.empty()) cfg->logging.level = log_level;
    auto rate_limit = extract_uint32(content, "rate_limit_ms");
    if (rate_limit) cfg->logging.rate_limit_ms = rate_limit;

    // Symbols array
    auto blocks = extract_symbol_blocks(content);
    if (!blocks.empty()) {
        cfg->symbols.clear();
        for (const auto& block : blocks) {
            SymbolConfig sc;
            sc.symbol          = extract_string(block, "symbol");
            sc.tick_size       = extract_double(block, "tick_size");
            sc.lot_size        = extract_uint64(block, "lot_size");
            sc.price_collar_pct= extract_double(block, "price_collar_pct");
            if (!sc.symbol.empty()) cfg->symbols.push_back(sc);
        }
    }

    return cfg;
}

} // namespace rtes
