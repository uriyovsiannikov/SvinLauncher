#include "http.hpp"
#include "../app/i18n.hpp"

#include <gio/gio.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace fs = std::filesystem;
namespace Minecraft::Internal {
namespace {
struct HttpSession {
    GSocketConnection* connection = nullptr;
    std::string host;

    void reset()
    {
        if (connection) {
            g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
            g_object_unref(connection);
            connection = nullptr;
        }
        host.clear();
    }
    ~HttpSession() { reset(); }
};

size_t completeChunkedSize(std::string_view data)
{
    size_t pos = 0;
    while (true) {
        const auto end = data.find("\r\n", pos);
        if (end == std::string_view::npos) return std::string::npos;
        auto line = data.substr(pos, end - pos);
        if (const auto semicolon = line.find(';'); semicolon != std::string_view::npos)
            line = line.substr(0, semicolon);
        size_t chunk_size = 0;
        const auto parsed = std::from_chars(line.data(), line.data() + line.size(), chunk_size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size()) return std::string::npos;
        pos = end + 2;
        if (chunk_size > data.size() - pos || data.size() - pos - chunk_size < 2) return std::string::npos;
        pos += chunk_size;
        if (data.substr(pos, 2) != "\r\n") return std::string::npos;
        pos += 2;
        if (chunk_size == 0) return pos;
    }
}

bool httpGet(std::string url, std::string& body, std::string& error, unsigned redirects = 0,
             const std::function<void(size_t, size_t)>& progress = {}, bool allow_unclean_tls_eof = false,
             HttpSession* session = nullptr, bool retry_reused = true)
{
    if (redirects > 5 || url.rfind("https://", 0) != 0) { error = tr("Недопустимый URL загрузки Minecraft"); return false; }
    const auto host_start = 8u;
    const auto path_start = url.find('/', host_start);
    auto host = url.substr(host_start, path_start == std::string::npos ? std::string::npos : path_start - host_start);
    auto path = path_start == std::string::npos ? std::string("/") : url.substr(path_start);
    if (host.empty() || host.find('@') != std::string::npos) { error = tr("Недопустимый сервер загрузки Minecraft"); return false; }
    if (session && session->host != host) session->reset();
    const bool reused = session && session->connection;
    GError* gio_error = nullptr;
    auto* connection = session ? session->connection : nullptr;
    if (!connection) {
        auto* client = g_socket_client_new();
        g_socket_client_set_tls(client, TRUE);
        g_socket_client_set_timeout(client, 45);
        connection = g_socket_client_connect_to_host(client, host.c_str(), 443, nullptr, &gio_error);
        g_object_unref(client);
        if (!connection) {
            error = (gio_error ? gio_error->message : tr("Не удалось подключиться к серверу Minecraft")) +
                    std::string(" (") + url + ")";
            if (gio_error) g_error_free(gio_error);
            return false;
        }
        if (session) {
            session->connection = connection;
            session->host = host;
        }
    }
    g_socket_set_timeout(g_socket_connection_get_socket(connection), 45);
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
        "\r\nUser-Agent: SvinLauncher/1.0\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: " +
        (session ? "keep-alive\r\n\r\n" : "close\r\n\r\n");
    gsize written = 0;
    if (!g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)), request.data(), request.size(),
                                   &written, nullptr, &gio_error)) {
        error = gio_error ? gio_error->message : tr("Не удалось отправить запрос серверу Minecraft");
    }
    std::string response;
    char buffer[32768];
    size_t expected_response_size = std::string::npos;
    size_t header_end = std::string::npos;
    bool chunked = false;
    bool server_closes = false;
    size_t last_reported_bytes = 0;
    bool unclean_tls_eof = false;
    auto* input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    while (error.empty()) {
        auto count = g_input_stream_read(input, buffer, sizeof(buffer), nullptr, &gio_error);
        if (count < 0) {
            const std::string message = gio_error ? gio_error->message : tr("Ошибка загрузки файлов Minecraft");
            if (allow_unclean_tls_eof && message.find("TLS connection was non-properly terminated") != std::string::npos) {
                unclean_tls_eof = true;
                break;
            }
            error = message + " (" + url + ")";
            break;
        }
        if (count == 0) break;
        response.append(buffer, static_cast<size_t>(count));
        if (response.size() > 128ull * 1024 * 1024) { error = tr("Ответ сервера Minecraft слишком большой"); break; }
        if (header_end == std::string::npos) {
            header_end = response.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                auto headers = response.substr(0, header_end);
                std::transform(headers.begin(), headers.end(), headers.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                chunked = headers.find("\r\ntransfer-encoding: chunked") != std::string::npos;
                server_closes = headers.find("\r\nconnection: close") != std::string::npos;
                const auto key = headers.find("\r\ncontent-length:");
                if (!chunked && key != std::string::npos) {
                    auto value_start = key + sizeof("\r\ncontent-length:") - 1;
                    while (value_start < headers.size() && std::isspace(static_cast<unsigned char>(headers[value_start])))
                        ++value_start;
                    const auto value_end = headers.find("\r\n", value_start);
                    size_t body_size = 0;
                    const auto parsed = std::from_chars(headers.data() + value_start,
                        headers.data() + (value_end == std::string::npos ? headers.size() : value_end), body_size);
                    if (parsed.ec == std::errc{} && parsed.ptr == headers.data() +
                        (value_end == std::string::npos ? headers.size() : value_end) &&
                        header_end + 4 <= 128ull * 1024 * 1024 &&
                        body_size <= 128ull * 1024 * 1024 - (header_end + 4))
                        expected_response_size = header_end + 4 + body_size;
                }
            }
        }
        if (chunked && expected_response_size == std::string::npos) {
            const auto size = completeChunkedSize(std::string_view(response).substr(header_end + 4));
            if (size != std::string::npos) expected_response_size = header_end + 4 + size;
        }
        if (progress && header_end != std::string::npos) {
            const size_t received = response.size() - header_end - 4;
            const size_t total = expected_response_size == std::string::npos ? 0 : expected_response_size - header_end - 4;
            if (received - last_reported_bytes >= 256 * 1024 || (total && received >= total)) {
                progress(received, total);
                last_reported_bytes = received;
            }
        }
        if (expected_response_size != std::string::npos && response.size() >= expected_response_size) break;
    }
    if (gio_error) { g_error_free(gio_error); gio_error = nullptr; }
    if (error.empty() && expected_response_size != std::string::npos && response.size() != expected_response_size)
        error = std::string(tr("Обрезан ответ Minecraft")) + " (" + url + ")";
    const bool keep_connection = session && error.empty() && !server_closes &&
        expected_response_size != std::string::npos && response.size() == expected_response_size;
    if (session) {
        if (!keep_connection) session->reset();
    } else {
        g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
        g_object_unref(connection);
    }
    if (!error.empty()) {
        if (reused && retry_reused) {
            error.clear();
            return httpGet(url, body, error, redirects, progress, allow_unclean_tls_eof, session, false);
        }
        return false;
    }

    auto split = response.find("\r\n\r\n");
    if (split == std::string::npos) {
        // An idle keep-alive connection may have been closed by the server.
        // Reconnect immediately instead of waiting for the outer retry delay.
        if (reused && retry_reused)
            return httpGet(url, body, error, redirects, progress, allow_unclean_tls_eof, session, false);
        error = tr("Некорректный ответ сервера Minecraft");
        return false;
    }
    const auto headers_end = split;
    auto line_end = response.find("\r\n");
    const auto status_line = response.substr(0, line_end);
    if (status_line.find(" 301 ") != std::string::npos || status_line.find(" 302 ") != std::string::npos ||
        status_line.find(" 307 ") != std::string::npos || status_line.find(" 308 ") != std::string::npos) {
        auto raw_headers = response.substr(0, headers_end);
        auto lowered_headers = raw_headers;
        std::transform(lowered_headers.begin(), lowered_headers.end(), lowered_headers.begin(), [](unsigned char c) { return std::tolower(c); });
        auto location_key = lowered_headers.find("\r\nlocation: ");
        if (location_key == std::string::npos) { error = tr("Сервер Minecraft прислал redirect без адреса"); return false; }
        location_key += 12;
        auto location_end = raw_headers.find("\r\n", location_key);
        auto location = raw_headers.substr(location_key, location_end - location_key);
        if (location.rfind("https://", 0) != 0) { error = tr("Сервер Minecraft прислал небезопасный адрес"); return false; }
        return httpGet(location, body, error, redirects + 1, progress, allow_unclean_tls_eof, session);
    }
    if (status_line.find(" 200 ") == std::string::npos) {
        // Put the URL before the server's verbose error text. AdwToast shortens
        // long titles, and the useful part of this error must remain visible.
        error = std::string(tr("Ошибка загрузки")) + " " + url + " (" + status_line + ")";
        return false;
    }
    auto headers = response.substr(0, headers_end);
    std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return std::tolower(c); });
    auto data = std::string_view(response).substr(split + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t pos = 0;
        while (pos < data.size()) {
            auto end = data.find("\r\n", pos);
            if (end == std::string_view::npos) { error = tr("Повреждён chunked-ответ Minecraft"); return false; }
            auto line = data.substr(pos, end - pos);
            auto semicolon = line.find(';'); if (semicolon != std::string_view::npos) line = line.substr(0, semicolon);
            size_t size = 0;
            auto parsed = std::from_chars(line.data(), line.data() + line.size(), size, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size()) { error = tr("Повреждён chunked-ответ Minecraft"); return false; }
            pos = end + 2;
            if (size == 0) break;
            if (pos + size + 2 > data.size() || data.substr(pos + size, 2) != "\r\n") { error = tr("Обрезан ответ Minecraft"); return false; }
            decoded.append(data.data() + pos, size); pos += size + 2;
        }
        body = std::move(decoded);
    } else body.assign(data);
    return true;
}
}
bool get(const std::string& url, std::string& body, std::string& error)
{
    return httpGet(url, body, error);
}
bool parseJson(std::string_view text, json::Value& root, std::string& error)
{
    json::Parser parser(text);
    if (!parser.parse(root)) { error = tr("Не удалось разобрать манифест Minecraft"); return false; }
    return true;
}
std::string stringField(const json::Value* object, std::string_view key)
{
    if (!object) return {};
    auto* value = object->get(key);
    return value ? value->string() : std::string{};
}
uint64_t numberField(const json::Value* object, std::string_view key)
{
    const auto text = stringField(object, key);
    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? value : 0;
}
void reportProgress(const ProgressCallback& progress, const std::string& stage,
            std::string current_file, size_t completed_files, size_t total_files,
            uint64_t transferred_bytes, uint64_t total_bytes)
{
    if (progress) progress({stage, std::move(current_file), completed_files, total_files,
                            transferred_bytes, total_bytes});
}
bool saveFile(const fs::path& path, const std::string& bytes, std::string& error)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) { error = std::string(tr("Не удалось создать каталог: ")) + ec.message(); return false; }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) { error = std::string(tr("Не удалось записать файл ")) + path.filename().string(); return false; }
    return true;
}
bool hasSha1(std::string_view bytes, std::string_view expected)
{
    auto* checksum = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(checksum, reinterpret_cast<const guchar*>(bytes.data()), bytes.size());
    const bool valid = expected == g_checksum_get_string(checksum);
    g_checksum_free(checksum);
    return valid;
}

static bool downloadWithSession(std::string url, const fs::path& target, std::string& error, std::string_view sha1 = {},
              const std::function<void(size_t, size_t)>& progress = {}, HttpSession* session = nullptr)
{
    if (fs::is_regular_file(target)) {
        if (sha1.empty()) return true;
        std::ifstream cached(target, std::ios::binary);
        auto* checksum = g_checksum_new(G_CHECKSUM_SHA1);
        char buffer[65536];
        while (cached) {
            cached.read(buffer, sizeof(buffer));
            const auto count = cached.gcount();
            if (count > 0) g_checksum_update(checksum, reinterpret_cast<const guchar*>(buffer), static_cast<gsize>(count));
        }
        const bool valid = cached.eof() && sha1 == g_checksum_get_string(checksum);
        g_checksum_free(checksum);
        if (valid) return true;
        std::error_code ec;
        fs::remove(target, ec);
    }
    if (url.rfind("http://", 0) == 0) url.replace(0, 7, "https://");
    std::string bytes;
    bool fetched = false;
    for (unsigned attempt = 0; attempt < 7; ++attempt) {
        error.clear();
        bytes.clear();
        if (httpGet(url, bytes, error, 0, progress, !sha1.empty(), session)) {
            if (sha1.empty() || hasSha1(bytes, sha1)) { fetched = true; break; }
            error = std::string(tr("Проверка SHA-1 не прошла для ")) + target.filename().string();
        }
        if (attempt < 6) g_usleep(500000u << std::min(attempt, 3u));
    }
    if (!fetched) return false;
    return saveFile(target, bytes, error);
}
bool safeRelativePath(const fs::path& path)
{
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& component : path) if (component == ".." || component == ".") return false;
    return true;
}
bool isSha1(std::string_view hash)
{
    return hash.size() == 40 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) { return std::isxdigit(c); });
}
bool downloadMany(const std::vector<FileDownload>& files, std::string& error,
                  const Minecraft::ProgressCallback& progress, const std::string& stage)
{
    if (files.empty()) return true;
    std::atomic_size_t next{0};
    std::mutex error_mutex;
    std::mutex progress_mutex;
    size_t completed_files = 0;
    uint64_t completed_bytes = 0;
    uint64_t total_bytes = 0;
    for (const auto& file : files) total_bytes += file.size;
    // Each worker handles its requests sequentially and keeps its own TLS session.
    constexpr size_t max_parallel_downloads = 8;
    const size_t thread_count = std::min(max_parallel_downloads, files.size());
    std::vector<uint64_t> active_bytes(thread_count, 0);
    reportProgress(progress, stage, {}, 0, files.size(), 0, total_bytes);
    const auto worker = [&](size_t worker_index) {
        HttpSession session;
        while (true) {
            const auto index = next.fetch_add(1);
            if (index >= files.size()) return;
            { std::lock_guard lock(error_mutex); if (!error.empty()) return; }
            std::string local_error;
            const auto& file = files[index];
            const auto on_bytes = [&](size_t received, size_t) {
                std::lock_guard lock(progress_mutex);
                active_bytes[worker_index] = file.size ? std::min<uint64_t>(received, file.size) : received;
                uint64_t current_bytes = completed_bytes;
                for (const auto count : active_bytes) current_bytes += count;
                reportProgress(progress, stage, file.display_name, completed_files, files.size(), current_bytes, total_bytes);
            };
            if (!downloadWithSession(file.url, file.target, local_error, file.sha1, on_bytes, &session)) {
                std::lock_guard lock(error_mutex);
                if (error.empty()) error = std::move(local_error);
                return;
            }
            if (progress) {
                std::lock_guard lock(progress_mutex);
                completed_bytes += file.size ? file.size : active_bytes[worker_index];
                active_bytes[worker_index] = 0;
                ++completed_files;
                uint64_t current_bytes = completed_bytes;
                for (const auto count : active_bytes) current_bytes += count;
                reportProgress(progress, stage, file.display_name, completed_files, files.size(), current_bytes, total_bytes);
            }
        }
    };
    std::vector<std::thread> workers;
    for (size_t i = 0; i < thread_count; ++i) workers.emplace_back(worker, i);
    for (auto& thread : workers) thread.join();
    return error.empty();
}

bool downloadFile(std::string url, const fs::path& target, std::string& error,
                  std::string_view sha1, const std::function<void(size_t, size_t)>& progress)
{
    return downloadWithSession(std::move(url), target, error, sha1, progress, nullptr);
}
} // namespace Minecraft::Internal
