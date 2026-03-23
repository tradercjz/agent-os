// ============================================================
// AgentOS Multimodal Demo
// Demonstrates creating messages with image and audio
// attachments, using the builder API, and showing the
// JSON format sent to the OpenAI-compatible API.
// ============================================================
#include <agentos/agentos.hpp>
#include <iostream>

using namespace agentos;
using namespace agentos::kernel;

// ── Helper formatting ──────────────────────────────────────

void section(std::string_view title) {
    std::cout << "\n== " << title << " ==\n";
}

void info(std::string_view msg) {
    std::cout << "  " << msg << "\n";
}

void print_json(const nlohmann::json& j) {
    std::cout << j.dump(2) << "\n";
}

// ── Part 1: Image message via factory method ───────────────

void demo_image_url() {
    section("1. Image Message (URL-based)");
    info("Use Message::user_with_image() to attach a remote image.\n");

    // Create a user message with an image URL attachment.
    // This is the simplest way to send an image to a vision model.
    auto msg = Message::user_with_image(
        "What is in this image? Describe it in detail.",
        "https://example.com/photos/sunset-over-ocean.png"
    );

    // Inspect the message structure
    std::cout << "  Message role: User\n";
    std::cout << "  Text content: \"" << msg.content << "\"\n";
    std::cout << "  Attachments: " << msg.attachments.size() << "\n";

    const auto& att = msg.attachments[0];
    std::cout << "  Attachment[0]:\n";
    std::cout << "    type:      Image\n";
    std::cout << "    mime_type: " << att.mime_type << "\n";
    std::cout << "    url:       " << att.url << "\n";
    std::cout << "    is_base64: " << (att.is_base64 ? "true" : "false") << "\n";

    // Show the JSON that would be sent to the OpenAI API.
    // The content field becomes an array with text + image_url objects.
    std::cout << "\n  OpenAI API JSON format:\n";
    nlohmann::json content_arr = nlohmann::json::array();
    content_arr.push_back({{"type", "text"}, {"text", msg.content}});
    content_arr.push_back({
        {"type", "image_url"},
        {"image_url", {{"url", att.url}}}
    });
    nlohmann::json msg_json = {
        {"role", "user"},
        {"content", content_arr}
    };
    print_json(msg_json);
}

// ── Part 2: Image message with base64 data ─────────────────

void demo_image_base64() {
    section("2. Image Message (Base64-encoded)");
    info("For local images, embed the data directly as base64.\n");

    // In a real application, you would read the file and base64-encode it.
    // Here we use a short placeholder to illustrate the pattern.
    std::string fake_b64_data = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAA"
                                 "fFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==";

    // Build a message with a base64 image attachment manually.
    Message msg = Message::user("Identify any text in this screenshot.");
    msg.attachments.push_back(ContentPart{
        .type = ContentType::Image,
        .data = fake_b64_data,
        .mime_type = "image/png",
        .url = {},
        .is_base64 = true,
    });

    std::cout << "  Text: \"" << msg.content << "\"\n";
    std::cout << "  Attachment: base64 PNG (" << fake_b64_data.size() << " chars)\n";

    // Show the JSON format with data URI
    std::cout << "\n  OpenAI API JSON format (data URI):\n";
    nlohmann::json content_arr = nlohmann::json::array();
    content_arr.push_back({{"type", "text"}, {"text", msg.content}});
    std::string data_uri = "data:" + msg.attachments[0].mime_type
                         + ";base64," + msg.attachments[0].data;
    content_arr.push_back({
        {"type", "image_url"},
        {"image_url", {{"url", data_uri}}}
    });
    nlohmann::json msg_json = {
        {"role", "user"},
        {"content", content_arr}
    };
    print_json(msg_json);
}

// ── Part 3: Audio message ──────────────────────────────────

void demo_audio() {
    section("3. Audio Message (Base64-encoded WAV)");
    info("Use Message::user_with_audio() for speech/audio input.\n");

    // In practice, audio_data would be the base64-encoded contents
    // of a WAV, MP3, or other audio file.
    std::string fake_audio_b64 = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAgD4AAIA+AAAB";

    auto msg = Message::user_with_audio(
        "Transcribe this audio clip and summarize its content.",
        fake_audio_b64,
        "audio/wav"  // MIME type (default is audio/wav)
    );

    std::cout << "  Text: \"" << msg.content << "\"\n";
    std::cout << "  Attachment: base64 audio/wav ("
              << fake_audio_b64.size() << " chars)\n";
    std::cout << "  is_base64: "
              << (msg.attachments[0].is_base64 ? "true" : "false") << "\n";

    // Show the OpenAI input_audio JSON format
    std::cout << "\n  OpenAI API JSON format:\n";
    nlohmann::json content_arr = nlohmann::json::array();
    content_arr.push_back({{"type", "text"}, {"text", msg.content}});
    content_arr.push_back({
        {"type", "input_audio"},
        {"input_audio", {
            {"data", msg.attachments[0].data},
            {"format", msg.attachments[0].mime_type}
        }}
    });
    nlohmann::json msg_json = {
        {"role", "user"},
        {"content", content_arr}
    };
    print_json(msg_json);
}

// ── Part 4: Builder API for multimodal requests ────────────

void demo_builder_api() {
    section("4. Builder API for Multimodal Requests");
    info("LLMRequest::builder() provides a fluent way to construct requests.\n");

    // Build a multimodal request using the builder pattern.
    // The .attachments() call adds attachments to the most recently
    // added message.
    auto request = LLMRequest::builder()
        .system("You are a helpful vision assistant.")
        .user("Describe this image and compare it with the audio.")
        .attachments({
            ContentPart{
                .type = ContentType::Image,
                .data = {},
                .mime_type = "image/jpeg",
                .url = "https://example.com/photo.jpg",
                .is_base64 = false,
            },
            ContentPart{
                .type = ContentType::Audio,
                .data = "UklGRiQAAABXQVZF...",
                .mime_type = "audio/wav",
                .url = {},
                .is_base64 = true,
            },
        })
        .model("gpt-4o")
        .temperature(0.3f)
        .max_tokens(1024)
        .build();

    std::cout << "  Request summary:\n";
    std::cout << "    Model: " << request.model << "\n";
    std::cout << "    Temperature: " << request.temperature << "\n";
    std::cout << "    Max tokens: " << request.max_tokens << "\n";
    std::cout << "    Messages: " << request.messages.size() << "\n";

    for (size_t i = 0; i < request.messages.size(); ++i) {
        const auto& m = request.messages[i];
        std::string role_str;
        switch (m.role) {
            case Role::System:    role_str = "system";    break;
            case Role::User:      role_str = "user";      break;
            case Role::Assistant: role_str = "assistant";  break;
            case Role::Tool:      role_str = "tool";      break;
        }
        std::cout << "    [" << i << "] role=" << role_str
                  << ", content=\"" << m.content << "\""
                  << ", attachments=" << m.attachments.size() << "\n";
    }
}

// ── Part 5: Send multimodal request through the kernel ─────

void demo_kernel_inference() {
    section("5. Multimodal Inference via LLMKernel");
    info("Send a multimodal request through the mock kernel.\n");

    // Create a mock backend with a rule that responds to image queries.
    auto mock = std::make_unique<MockLLMBackend>("mock-vision");
    mock->register_rule(
        "image",
        "I can see a beautiful sunset over the ocean. The sky has "
        "vibrant orange and purple hues reflected on the water surface.",
        /*priority=*/10);
    mock->register_rule(
        "audio",
        "The audio contains a person saying: 'Welcome to the AgentOS "
        "multimodal demo. This system supports images and audio.'",
        /*priority=*/10);

    // Build AgentOS with the mock backend
    auto os = AgentOSBuilder()
                  .backend(std::move(mock))
                  .security(false)
                  .build();

    // --- Image request ---
    info("Sending image request...");
    auto img_msg = Message::user_with_image(
        "Describe this image.",
        "https://example.com/sunset.jpg"
    );

    LLMRequest img_req;
    img_req.messages = {
        Message::system("You are a vision assistant."),
        std::move(img_msg),
    };

    auto img_result = os->kernel().infer(img_req);
    if (img_result.has_value()) {
        std::cout << "  Response: " << img_result->content << "\n";
        std::cout << "  Tokens: prompt=" << img_result->prompt_tokens
                  << " completion=" << img_result->completion_tokens
                  << " total=" << img_result->total_tokens() << "\n";
    } else {
        std::cout << "  Error: " << img_result.error().message << "\n";
    }

    // --- Audio request ---
    std::cout << "\n";
    info("Sending audio request...");
    auto audio_msg = Message::user_with_audio(
        "Transcribe this audio.",
        "UklGRiQAAABXQVZFZm10...",
        "audio/wav"
    );

    LLMRequest audio_req;
    audio_req.messages = {
        Message::system("You are a transcription assistant."),
        std::move(audio_msg),
    };

    auto audio_result = os->kernel().infer(audio_req);
    if (audio_result.has_value()) {
        std::cout << "  Response: " << audio_result->content << "\n";
        std::cout << "  Tokens: prompt=" << audio_result->prompt_tokens
                  << " completion=" << audio_result->completion_tokens
                  << " total=" << audio_result->total_tokens() << "\n";
    } else {
        std::cout << "  Error: " << audio_result.error().message << "\n";
    }
}

// ── Part 6: Multiple attachments in one message ────────────

void demo_multiple_attachments() {
    section("6. Multiple Attachments in One Message");
    info("A single message can carry multiple images or mixed media.\n");

    Message msg = Message::user("Compare these two images side by side.");

    // Attach two images from URLs
    msg.attachments.push_back(ContentPart{
        .type = ContentType::Image,
        .data = {},
        .mime_type = "image/jpeg",
        .url = "https://example.com/before.jpg",
        .is_base64 = false,
    });
    msg.attachments.push_back(ContentPart{
        .type = ContentType::Image,
        .data = {},
        .mime_type = "image/jpeg",
        .url = "https://example.com/after.jpg",
        .is_base64 = false,
    });

    std::cout << "  Text: \"" << msg.content << "\"\n";
    std::cout << "  Attachments: " << msg.attachments.size() << "\n";
    for (size_t i = 0; i < msg.attachments.size(); ++i) {
        const auto& a = msg.attachments[i];
        std::cout << "    [" << i << "] type=Image, url=" << a.url << "\n";
    }

    // Show the full JSON content array
    std::cout << "\n  OpenAI API content array:\n";
    nlohmann::json content_arr = nlohmann::json::array();
    content_arr.push_back({{"type", "text"}, {"text", msg.content}});
    for (const auto& a : msg.attachments) {
        content_arr.push_back({
            {"type", "image_url"},
            {"image_url", {{"url", a.url}}}
        });
    }
    print_json(content_arr);
}

// ── Main ───────────────────────────────────────────────────

int main() {
    std::cout << "============================================\n";
    std::cout << " AgentOS Multimodal Demo\n";
    std::cout << "============================================\n";

    // Part 1: URL-based image message
    demo_image_url();

    // Part 2: Base64-encoded image
    demo_image_base64();

    // Part 3: Audio attachment
    demo_audio();

    // Part 4: Builder API for multimodal requests
    demo_builder_api();

    // Part 5: Send through the LLM kernel
    demo_kernel_inference();

    // Part 6: Multiple attachments
    demo_multiple_attachments();

    std::cout << "\nMultimodal demo complete.\n";
    return 0;
}
