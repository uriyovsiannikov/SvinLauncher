#include "version_catalog.hpp"
#include "../app/i18n.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <string_view>
#include <utility>

namespace Minecraft::VersionCatalog {
static void skip_json_space(std::string_view json, size_t& pos)
{
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
}

static bool read_json_string(std::string_view json, size_t& pos, std::string* output)
{
    if (pos >= json.size() || json[pos++] != '"') return false;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            if (output) output->push_back(c);
            continue;
        }
        if (pos >= json.size()) return false;
        c = json[pos++];
        switch (c) {
            case '"': case '\\': case '/': if (output) output->push_back(c); break;
            case 'b': if (output) output->push_back('\b'); break;
            case 'f': if (output) output->push_back('\f'); break;
            case 'n': if (output) output->push_back('\n'); break;
            case 'r': if (output) output->push_back('\r'); break;
            case 't': if (output) output->push_back('\t'); break;
            case 'u':
                if (pos + 4 > json.size()) return false;
                if (output) output->push_back('?');
                pos += 4;
                break;
            default: return false;
        }
    }
    return false;
}

static bool skip_json_value(std::string_view json, size_t& pos)
{
    skip_json_space(json, pos);
    if (pos >= json.size()) return false;
    if (json[pos] == '"') return read_json_string(json, pos, nullptr);
    if (json[pos] == '{' || json[pos] == '[') {
        const char closing = json[pos++] == '{' ? '}' : ']';
        while (pos < json.size()) {
            skip_json_space(json, pos);
            if (json[pos] == closing) { ++pos; return true; }
            if (json[pos] == ',' || json[pos] == ':') { ++pos; continue; }
            if (json[pos] == '"') {
                if (!read_json_string(json, pos, nullptr)) return false;
            } else if (!skip_json_value(json, pos)) {
                return false;
            }
        }
        return false;
    }
    const size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']' &&
           !std::isspace(static_cast<unsigned char>(json[pos]))) ++pos;
    return pos > start;
}

static bool parse_release_versions(std::string_view json, std::vector<std::string>& versions)
{
    auto key = json.find("\"versions\"");
    if (key == std::string_view::npos) return false;
    size_t pos = key + 10;
    skip_json_space(json, pos);
    if (pos >= json.size() || json[pos++] != ':') return false;
    skip_json_space(json, pos);
    if (pos >= json.size() || json[pos++] != '[') return false;

    while (pos < json.size()) {
        skip_json_space(json, pos);
        if (json[pos] == ']') return true;
        if (json[pos++] != '{') return false;
        std::string id, type;
        bool has_id = false, has_type = false;
        while (pos < json.size()) {
            skip_json_space(json, pos);
            if (json[pos] == '}') { ++pos; break; }
            std::string field;
            if (!read_json_string(json, pos, &field)) return false;
            skip_json_space(json, pos);
            if (pos >= json.size() || json[pos++] != ':') return false;
            skip_json_space(json, pos);
            if (field == "id") has_id = read_json_string(json, pos, &id);
            else if (field == "type") has_type = read_json_string(json, pos, &type);
            else if (!skip_json_value(json, pos)) return false;
            if ((field == "id" && !has_id) || (field == "type" && !has_type)) return false;
            skip_json_space(json, pos);
            if (pos < json.size() && json[pos] == ',') ++pos;
        }
        if (has_id && has_type && type == "release") versions.push_back(std::move(id));
        skip_json_space(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
        else if (pos < json.size() && json[pos] == ']') return true;
        else return false;
    }
    return false;
}

static bool decode_chunked_body(std::string_view body, std::string& output)
{
    size_t pos = 0;
    while (pos < body.size()) {
        auto line_end = body.find("\r\n", pos);
        if (line_end == std::string_view::npos) return false;
        auto chunk_header = body.substr(pos, line_end - pos);
        auto extension = chunk_header.find(';');
        if (extension != std::string_view::npos) chunk_header = chunk_header.substr(0, extension);
        size_t chunk_size = 0;
        auto parsed = std::from_chars(chunk_header.data(), chunk_header.data() + chunk_header.size(), chunk_size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != chunk_header.data() + chunk_header.size()) return false;
        pos = line_end + 2;
        if (chunk_size == 0) return true;
        if (pos + chunk_size + 2 > body.size() || body.substr(pos + chunk_size, 2) != "\r\n") return false;
        output.append(body.data() + pos, chunk_size);
        pos += chunk_size + 2;
    }
    return false;
}

Result fetch_releases(GCancellable* cancellable)
{
    Result result;
    GError* error = nullptr;
    auto* client = g_socket_client_new();
    g_socket_client_set_tls(client, TRUE);
    g_socket_client_set_timeout(client, 15);
    auto* connection = g_socket_client_connect_to_host(client, "piston-meta.mojang.com", 443, cancellable, &error);
    if (!connection) {
        result.error = error ? error->message : tr("Не удалось подключиться к серверу версий Minecraft");
        if (error) g_error_free(error);
        g_object_unref(client);
        return result;
    }
    g_socket_set_timeout(g_socket_connection_get_socket(connection), 15);

    const std::string request = "GET /mc/game/version_manifest_v2.json HTTP/1.1\r\n"
                                "Host: piston-meta.mojang.com\r\n"
                                "User-Agent: SvinLauncher/1.0\r\n"
                                "Accept: application/json\r\n"
                                "Accept-Encoding: identity\r\n"
                                "Connection: close\r\n\r\n";
    gsize written = 0;
    if (!g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)), request.data(), request.size(),
                                   &written, cancellable, &error)) {
        result.error = error ? error->message : tr("Не удалось отправить запрос списка версий");
    }

    std::string response;
    char buffer[8192];
    auto* input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    while (result.error.empty()) {
        auto count = g_input_stream_read(input, buffer, sizeof(buffer), cancellable, &error);
        if (count < 0) {
            result.error = error ? error->message : tr("Ошибка чтения списка версий");
            break;
        }
        if (count == 0) break;
        response.append(buffer, static_cast<size_t>(count));
        if (response.size() > 16 * 1024 * 1024) {
            result.error = tr("Ответ со списком версий слишком большой");
            break;
        }
    }
    if (error) { g_error_free(error); error = nullptr; }
    g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
    g_object_unref(connection);
    g_object_unref(client);
    if (!result.error.empty()) return result;

    const auto separator = response.find("\r\n\r\n");
    if (separator == std::string::npos || (response.compare(0, 12, "HTTP/1.1 200") != 0 &&
        response.compare(0, 12, "HTTP/1.0 200") != 0)) {
        result.error = tr("Сервер версий Minecraft вернул некорректный ответ");
        return result;
    }
    auto headers = response.substr(0, separator);
    std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return std::tolower(c); });
    auto body = std::string_view(response).substr(separator + 4);
    std::string decoded_body;
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        if (!decode_chunked_body(body, decoded_body)) {
            result.error = tr("Не удалось прочитать ответ сервера версий Minecraft");
            return result;
        }
        body = decoded_body;
    }
    if (!parse_release_versions(body, result.versions) || result.versions.empty())
        result.error = tr("Не удалось разобрать список версий Minecraft");
    return result;
}


} // namespace Minecraft::VersionCatalog
