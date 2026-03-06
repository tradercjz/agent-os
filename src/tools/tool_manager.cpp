#include <agentos/tools/tool_manager.hpp>
#include <curl/curl.h>

namespace agentos::tools {

// ---- ShellTool Implementation ----
ToolResult ShellTool::execute(const ParsedArgs &args) {
  auto cmd = args.get("cmd");
  // 提取第一个词（命令名）
  std::string cmd_name = cmd.substr(0, cmd.find(' '));
  if (!allowed_cmds_.count(cmd_name)) {
    return ToolResult::fail(
        fmt::format("Command '{}' not in allowlist", cmd_name));
  }

  // 简单防注入：禁止 shell 元字符
  std::string unsafe_chars = "&|;$\n\r`()<>\\";
  if (cmd.find_first_of(unsafe_chars) != std::string::npos) {
    return ToolResult::fail(
        "Unsafe characters detected in command. Shell expansions, redirection, "
        "and chaining are not allowed.");
  }

  // 限制输出长度
  std::string safe_cmd = cmd + " 2>&1 | head -100";
  std::array<char, 2048> buf{};
  std::string output;
  FILE *pipe = popen(safe_cmd.c_str(), "r");
  if (!pipe)
    return ToolResult::fail("Failed to open pipe");
  while (fgets(buf.data(), buf.size(), pipe) != nullptr)
    output += buf.data();
  pclose(pipe);

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

ToolResult HttpFetchTool::execute(const ParsedArgs &args) {
  auto url = args.get("url");
  if (url.empty())
    return ToolResult::fail("URL is required");
  if (url.substr(0, 4) != "http")
    return ToolResult::fail("Only http/https URLs are allowed");

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
