#include <agentos/core/logger.hpp>
#include <agentos/tools/tool_manager.hpp>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/wait.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sstream>

namespace agentos::tools {

// ---- ShellTool Implementation ----
ToolResult ShellTool::execute(const ParsedArgs &args, std::stop_token st) {
  static constexpr size_t kMaxCommandLength = 4096;

  auto cmd = args.get("cmd");
  if (cmd.empty())
    return ToolResult::fail("Command must not be empty");

  // Check command length before proceeding
  if (cmd.size() > kMaxCommandLength) {
    LOG_WARN(fmt::format("ShellTool: command too long ({} > {} bytes)", cmd.size(), kMaxCommandLength));
    return ToolResult{.success = false, .output = "error: command too long", .error = ""};
  }

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

  // Production-grade: use fork/execvp instead of /bin/sh to avoid shell injection
  // execl("/bin/sh", "sh", "-c", ...) parses metacharacters; execvp() does not.
  // Split command into argv (program + first arg only) to prevent injection

  std::vector<std::string> parts;
  std::istringstream iss(cmd);
  std::string tok;
  while (iss >> tok) parts.push_back(tok);

  if (parts.empty()) {
    return ToolResult::fail("Command parsing failed");
  }

  std::vector<char *> argv;
  for (auto &p : parts) argv.push_back(p.data());
  argv.push_back(nullptr);

  int pipefd[2];
  if (pipe(pipefd) == -1) {
    return ToolResult{.success = false, .output = "", .error = "Failed to create pipe"};
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    return ToolResult{.success = false, .output = "", .error = "Failed to fork process"};
  }

  if (pid == 0) {
    // Child process: redirect stdout+stderr to pipe write end
    close(pipefd[0]);          // close read end
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    // Execute without shell: argv[0]=program, argv[1..n]=args, NO shell parsing
    execvp(argv[0], argv.data());
    _exit(127);  // execvp failed
  }

  // Parent process
  close(pipefd[1]);  // close write end

  std::string output;
  output.reserve(4096);
  char buf[4096];
  size_t line_count = 0;
  constexpr size_t kMaxOutputLines = 100;
  bool truncated = false;
  size_t total_bytes = 0;
  constexpr size_t kMaxOutputBytes = 1024 * 100; // 100 KB limit

  ssize_t n;
  struct pollfd pfd;
  pfd.fd = pipefd[0];
  pfd.events = POLLIN;

  while (true) {
    if (st.stop_requested()) {
      kill(pid, SIGKILL);
      truncated = true;
      break;
    }

    int poll_res = poll(&pfd, 1, 100); // 100ms timeout
    if (poll_res < 0) {
      if (errno == EINTR) continue;
      break;
    } else if (poll_res == 0) {
      // Timeout, check stop_token again
      continue;
    }

    n = read(pipefd[0], buf, sizeof(buf) - 1);
    if (n <= 0) break; // EOF or error

    buf[n] = '\0';
    // Count lines and check output size limit
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == '\n') ++line_count;
      if (line_count >= kMaxOutputLines) {
        truncated = true;
        break;
      }
    }
    // Respect output byte limit
    if (total_bytes + static_cast<size_t>(n) > kMaxOutputBytes) {
      output.append(buf, kMaxOutputBytes - total_bytes);
      truncated = true;
      break;
    }
    if (!truncated) {
      output.append(buf, n);
      total_bytes += n;
    } else {
      // Read remaining bytes to let child exit cleanly
      // but don't append to output
    }
  }
  close(pipefd[0]);

  int status;
  waitpid(pid, &status, 0);
  int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  if (truncated) {
    output += "\n[output truncated]";
  }

  if (exit_code != 0 && output.empty()) {
    return ToolResult{.success = false, .output = fmt::format("Command failed with exit code {}", exit_code), .error = "", .truncated = truncated};
  }
  return ToolResult{true, output, "", truncated};
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
  explicit CurlGuard(CURL *h) noexcept : handle(h) {}
  ~CurlGuard() noexcept { if (handle) curl_easy_cleanup(handle); }
  CurlGuard(const CurlGuard &) = delete;
  CurlGuard &operator=(const CurlGuard &) = delete;
  CurlGuard(CurlGuard &&other) noexcept : handle(other.handle) { other.handle = nullptr; }
  CurlGuard &operator=(CurlGuard &&other) noexcept {
    if (this != &other) { if (handle) curl_easy_cleanup(handle); handle = other.handle; other.handle = nullptr; }
    return *this;
  }
};

// SSRF protection: block requests to private/internal networks
static bool is_private_ip(const std::string &hostname) {
  // Block obvious private hostnames
  if (hostname == "localhost" || hostname == "127.0.0.1" ||
      hostname == "::1" || hostname == "0.0.0.0" ||
      hostname.ends_with(".local") || hostname.ends_with(".internal"))
    return true;

  // Resolve and check IP ranges
  // Replace manual freeaddrinfo with RAII guard to prevent leak on exception path
  struct AddrInfoGuard {
    struct addrinfo *ptr = nullptr;
    ~AddrInfoGuard() noexcept { if (ptr) freeaddrinfo(ptr); }
  } addr_guard;

  struct addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(hostname.c_str(), nullptr, &hints, &addr_guard.ptr) != 0)
    return true;  // Fail-safe: treat resolve failure as private

  bool is_private = false;
  // Use addr_guard.ptr instead of result
  for (auto *rp = addr_guard.ptr; rp != nullptr; rp = rp->ai_next) {
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
  // NO explicit freeaddrinfo needed — guard destructor handles it
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

static int curl_progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
  auto *st = static_cast<std::stop_token *>(clientp);
  if (st && st->stop_requested()) {
    return 1; // Return non-zero to abort the transfer
  }
  return 0;
}

ToolResult HttpFetchTool::execute(const ParsedArgs &args, std::stop_token st) {
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

  // Helper lambda to check curl_easy_setopt return codes
  auto check_opt = [&](CURLcode code, const char* opt_name) {
    if (code != CURLE_OK) {
      LOG_WARN(fmt::format("[HttpTool] curl_easy_setopt({}) failed: {}",
                           opt_name, curl_easy_strerror(code)));
    }
  };

  check_opt(curl_easy_setopt(raw, CURLOPT_URL, url.c_str()), "CURLOPT_URL");
  check_opt(curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, curl_write_callback), "CURLOPT_WRITEFUNCTION");
  check_opt(curl_easy_setopt(raw, CURLOPT_WRITEDATA, &output), "CURLOPT_WRITEDATA");
  check_opt(curl_easy_setopt(raw, CURLOPT_TIMEOUT, 10L), "CURLOPT_TIMEOUT");
  check_opt(curl_easy_setopt(raw, CURLOPT_FOLLOWLOCATION, 1L), "CURLOPT_FOLLOWLOCATION");
  check_opt(curl_easy_setopt(raw, CURLOPT_MAXREDIRS, 3L), "CURLOPT_MAXREDIRS");

  check_opt(curl_easy_setopt(raw, CURLOPT_NOPROGRESS, 0L), "CURLOPT_NOPROGRESS");
  check_opt(curl_easy_setopt(raw, CURLOPT_XFERINFOFUNCTION, curl_progress_callback), "CURLOPT_XFERINFOFUNCTION");
  check_opt(curl_easy_setopt(raw, CURLOPT_XFERINFODATA, &st), "CURLOPT_XFERINFODATA");

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
