#include <agentos/core/logger.hpp>
#include <agentos/tools/tool_manager.hpp>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netdb.h>

namespace agentos::tools {

// ---- ShellTool Implementation ----
ToolResult ShellTool::execute(const ParsedArgs &args) {
  auto cmd = args.get("cmd");
  if (cmd.empty())
    return ToolResult::fail("Command must not be empty");
  // 提取第一个词（命令名）
  std::string cmd_name = cmd.substr(0, cmd.find(' '));
  if (cmd_name.empty() || !allowed_cmds_.contains(cmd_name)) {
    return ToolResult::fail(
        fmt::format("Command '{}' not in allowlist", cmd_name));
  }

  // 防注入：禁止 null 字节和 shell 元字符
  if (cmd.find('\0') != std::string::npos) {
    LOG_WARN("ShellTool: blocked null byte in command");
    return ToolResult::fail("Null bytes are not allowed in commands.");
  }
  std::string unsafe_chars = "&|;$\n\r`()<>\\\"'{}[]!#~";
  if (cmd.find_first_of(unsafe_chars) != std::string::npos) {
    LOG_WARN(fmt::format("ShellTool: blocked unsafe characters in command '{}'", cmd_name));
    return ToolResult::fail(
        "Unsafe characters detected in command. Shell expansions, redirection, "
        "and chaining are not allowed.");
  }

  // 限制参数数量防止参数注入
  size_t arg_count = 0;
  bool in_space = true;
  for (char c : cmd) {
    if (c == ' ') { in_space = true; }
    else if (in_space) { arg_count++; in_space = false; }
  }
  if (arg_count > 20) {
    return ToolResult::fail("Too many arguments (max 20)");
  }

  // RAII wrapper for popen/pclose
  struct PipeGuard {
    FILE *fp;
    explicit PipeGuard(FILE *f) : fp(f) {}
    ~PipeGuard() { if (fp) pclose(fp); }
    PipeGuard(const PipeGuard &) = delete;
    PipeGuard &operator=(const PipeGuard &) = delete;
  };

  // 限制输出长度
  std::string safe_cmd = cmd + " 2>&1 | head -100";
  std::array<char, 2048> buf{};
  std::string output;
  FILE *pipe = popen(safe_cmd.c_str(), "r");
  if (!pipe)
    return ToolResult::fail("Failed to open pipe");
  PipeGuard guard(pipe);
  while (fgets(buf.data(), buf.size(), pipe) != nullptr)
    output += buf.data();

  // 截断过长输出，以防恶意产生巨大输出
  if (output.size() > 10240) {
    output = output.substr(0, 10240) + "\n...[truncated]";
  }
  return ToolResult::ok(output);
}

// ---- HttpFetchTool Implementation ----
static size_t curl_write_callback(void *contents, size_t size, size_t nmemb,
                                  void *userp) {
  auto *str = static_cast<std::string *>(userp);
  size_t new_len = size * nmemb;
  // 限制读取长度，防止 OOM (最大 100KB)
  if (str->size() + new_len > 102400) {
    return 0; // 返回 0 导致 curl 中止并报错 CURLE_WRITE_ERROR
  }
  str->append(static_cast<char *>(contents), new_len);
  return new_len;
}

// RAII wrapper for CURL handle，避免异常时泄漏
struct CurlGuard {
  CURL *handle;
  explicit CurlGuard(CURL *h) : handle(h) {}
  ~CurlGuard() { if (handle) curl_easy_cleanup(handle); }
  CurlGuard(const CurlGuard &) = delete;
  CurlGuard &operator=(const CurlGuard &) = delete;
};

// SSRF protection: block requests to private/internal networks
static bool is_private_ip(const std::string &hostname) {
  // Block obvious private hostnames
  if (hostname == "localhost" || hostname == "127.0.0.1" ||
      hostname == "::1" || hostname == "0.0.0.0" ||
      hostname.ends_with(".local") || hostname.ends_with(".internal"))
    return true;

  // Resolve and check IP ranges
  struct addrinfo hints{}, *result = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) != 0)
    return true; // Fail-closed: unresolvable = blocked

  bool is_private = false;
  for (auto *rp = result; rp != nullptr; rp = rp->ai_next) {
    if (rp->ai_family == AF_INET) {
      auto *addr = reinterpret_cast<struct sockaddr_in *>(rp->ai_addr);
      uint32_t ip = ntohl(addr->sin_addr.s_addr);
      // 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 127.0.0.0/8, 169.254.0.0/16
      if ((ip >> 24) == 10 ||
          (ip >> 20) == (172 << 4 | 1) ||  // 172.16-31.x.x
          (ip >> 16) == (192 << 8 | 168) ||
          (ip >> 24) == 127 ||
          (ip >> 16) == (169 << 8 | 254) ||
          ip == 0) {
        is_private = true;
        break;
      }
    } else if (rp->ai_family == AF_INET6) {
      auto *addr6 = reinterpret_cast<struct sockaddr_in6 *>(rp->ai_addr);
      auto *b = addr6->sin6_addr.s6_addr;
      // ::1 (loopback), fe80::/10 (link-local), fc00::/7 (ULA)
      if ((b[0] == 0 && b[1] == 0 && b[15] == 1) ||
          (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) ||
          (b[0] & 0xfe) == 0xfc) {
        is_private = true;
        break;
      }
    }
  }
  freeaddrinfo(result);
  return is_private;
}

// Extract hostname from URL
static std::string extract_hostname(const std::string &url) {
  // Skip scheme (http:// or https://)
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return "";
  auto host_start = scheme_end + 3;
  // Find end of host (port, path, or end)
  auto host_end = url.find_first_of(":/?#", host_start);
  if (host_end == std::string::npos) host_end = url.size();
  return url.substr(host_start, host_end - host_start);
}

ToolResult HttpFetchTool::execute(const ParsedArgs &args) {
  auto url = args.get("url");
  if (url.empty())
    return ToolResult::fail("URL is required");
  if (!url.starts_with("http"))
    return ToolResult::fail("Only http/https URLs are allowed");

  // SSRF protection: block private/internal network access
  std::string hostname = extract_hostname(url);
  if (hostname.empty())
    return ToolResult::fail("Cannot extract hostname from URL");
  if (is_private_ip(hostname)) {
    LOG_WARN(fmt::format("SSRF blocked: attempt to access private IP via '{}'", hostname));
    return ToolResult::fail("Access to private/internal networks is not allowed");
  }

  CURL *raw = curl_easy_init();
  if (!raw)
    return ToolResult::fail("Failed to initialize libcurl");
  CurlGuard guard(raw); // RAII 保证 cleanup

  std::string output;
  curl_easy_setopt(raw, CURLOPT_URL, url.c_str());
  curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, curl_write_callback);
  curl_easy_setopt(raw, CURLOPT_WRITEDATA, &output);
  curl_easy_setopt(raw, CURLOPT_TIMEOUT, 10L);       // 10s 超时
  curl_easy_setopt(raw, CURLOPT_FOLLOWLOCATION, 1L); // 允许重定向
  curl_easy_setopt(raw, CURLOPT_MAXREDIRS, 3L);

  CURLcode res = curl_easy_perform(raw);

  long response_code = 0;
  curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &response_code);

  if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
    return ToolResult::fail(fmt::format(
        "curl failed: {} (code: {})", curl_easy_strerror(res), response_code));
  }

  if (response_code == 0) {
    return ToolResult::fail("HTTP request failed: no response received");
  }
  if (response_code >= 400 && response_code < 600) {
    return ToolResult::fail(
        fmt::format("HTTP Request failed with status code {}", response_code));
  }

  if (output.size() > 4000) {
    output = output.substr(0, 4000) + "...[truncated]";
  }
  return ToolResult::ok(output);
}

} // namespace agentos::tools
