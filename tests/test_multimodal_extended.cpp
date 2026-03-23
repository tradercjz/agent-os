#include <agentos/kernel/llm_kernel.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace agentos;
using namespace agentos::kernel;
using Json = nlohmann::json;

// ─────────────────────────────────────────────────────────────
// 1. Message with multiple attachments (image + audio + text)
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, MultipleAttachments) {
  Message msg = Message::user("Analyze all of these");
  msg.attachments.push_back(ContentPart{
      .type = ContentType::Image,
      .data = {},
      .mime_type = "image/png",
      .url = "https://example.com/photo.png",
      .is_base64 = false,
  });
  msg.attachments.push_back(ContentPart{
      .type = ContentType::Audio,
      .data = "UklGRaudio",
      .mime_type = "audio/wav",
      .url = {},
      .is_base64 = true,
  });
  msg.attachments.push_back(ContentPart{
      .type = ContentType::Text,
      .data = "Supplementary text attachment",
      .mime_type = "text/plain",
      .url = {},
      .is_base64 = false,
  });

  ASSERT_EQ(msg.attachments.size(), 3u);
  EXPECT_EQ(msg.attachments[0].type, ContentType::Image);
  EXPECT_EQ(msg.attachments[1].type, ContentType::Audio);
  EXPECT_EQ(msg.attachments[2].type, ContentType::Text);
  EXPECT_EQ(msg.content, "Analyze all of these");
}

// ─────────────────────────────────────────────────────────────
// 2. ContentPart with base64 data
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, ContentPartBase64Data) {
  std::string b64 = "iVBORw0KGgoAAAANSUhEUgAAAAUA";
  ContentPart part{
      .type = ContentType::Image,
      .data = b64,
      .mime_type = "image/png",
      .url = {},
      .is_base64 = true,
  };
  EXPECT_TRUE(part.is_base64);
  EXPECT_EQ(part.data, b64);
  EXPECT_TRUE(part.url.empty());
}

// ─────────────────────────────────────────────────────────────
// 3. ContentPart with URL
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, ContentPartWithUrl) {
  ContentPart part{
      .type = ContentType::Image,
      .data = {},
      .mime_type = "image/jpeg",
      .url = "https://cdn.example.com/images/landscape.jpg",
      .is_base64 = false,
  };
  EXPECT_FALSE(part.is_base64);
  EXPECT_TRUE(part.data.empty());
  EXPECT_EQ(part.url, "https://cdn.example.com/images/landscape.jpg");
}

// ─────────────────────────────────────────────────────────────
// 4. ContentPart with all MIME types
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, AllMimeTypes) {
  struct TestCase {
    ContentType type;
    std::string mime;
  };
  std::vector<TestCase> cases = {
      {ContentType::Image, "image/png"},
      {ContentType::Image, "image/jpeg"},
      {ContentType::Audio, "audio/wav"},
      {ContentType::Audio, "audio/mp3"},
      {ContentType::Video, "video/mp4"},
  };

  for (const auto &tc : cases) {
    ContentPart part{
        .type = tc.type,
        .data = "somedata",
        .mime_type = tc.mime,
        .url = {},
        .is_base64 = true,
    };
    EXPECT_EQ(part.type, tc.type) << "Failed for mime: " << tc.mime;
    EXPECT_EQ(part.mime_type, tc.mime);
  }
}

// ─────────────────────────────────────────────────────────────
// 5. Empty attachments vector is handled correctly
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, EmptyAttachmentsHandled) {
  Message msg = Message::user("No attachments here");
  EXPECT_TRUE(msg.attachments.empty());

  // Build a request with empty attachments and run through MockBackend
  auto req = LLMRequest::builder()
                 .user("Plain text")
                 .build();
  ASSERT_EQ(req.messages.size(), 1u);
  EXPECT_TRUE(req.messages[0].attachments.empty());

  MockLLMBackend mock;
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->content.empty());
}

// ─────────────────────────────────────────────────────────────
// 6. LLMRequestBuilder chains: system -> user_with_image -> assistant
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, BuilderChainWithMultimodal) {
  auto req = LLMRequest::builder()
                 .system("You are a vision assistant")
                 .add_message(Message::user_with_image(
                     "What is in this photo?",
                     "https://example.com/photo.jpg"))
                 .assistant("It appears to be a cat.")
                 .model("gpt-4o")
                 .temperature(0.5f)
                 .max_tokens(1024)
                 .build();

  ASSERT_EQ(req.messages.size(), 3u);

  // System message
  EXPECT_EQ(req.messages[0].role, Role::System);
  EXPECT_EQ(req.messages[0].content, "You are a vision assistant");
  EXPECT_TRUE(req.messages[0].attachments.empty());

  // User message with image
  EXPECT_EQ(req.messages[1].role, Role::User);
  EXPECT_EQ(req.messages[1].content, "What is in this photo?");
  ASSERT_EQ(req.messages[1].attachments.size(), 1u);
  EXPECT_EQ(req.messages[1].attachments[0].type, ContentType::Image);
  EXPECT_EQ(req.messages[1].attachments[0].url, "https://example.com/photo.jpg");

  // Assistant message
  EXPECT_EQ(req.messages[2].role, Role::Assistant);
  EXPECT_EQ(req.messages[2].content, "It appears to be a cat.");
  EXPECT_TRUE(req.messages[2].attachments.empty());

  EXPECT_EQ(req.model, "gpt-4o");
  EXPECT_FLOAT_EQ(req.temperature, 0.5f);
  EXPECT_EQ(req.max_tokens, 1024u);
}

// ─────────────────────────────────────────────────────────────
// 7. Token estimation with multimodal messages
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, TokenEstimationMultimodal) {
  auto msg = Message::user_with_image("Describe this", "https://example.com/img.png");
  // Token estimation is based on text content only (size/4 + 1 + 4 overhead)
  TokenCount tokens = msg.tokens();
  EXPECT_GT(tokens, 0u);

  // A message with more text should estimate more tokens
  auto msg2 = Message::user_with_image(
      "This is a much longer description that should produce more tokens",
      "https://example.com/img.png");
  EXPECT_GT(msg2.tokens(), msg.tokens());

  // Empty content message should return 0 (no estimation on empty)
  Message empty_msg;
  empty_msg.role = Role::User;
  empty_msg.content = "";
  EXPECT_EQ(empty_msg.tokens(), 0u);
}

// ─────────────────────────────────────────────────────────────
// 8. JSON serialization of mixed content (text + image + audio)
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, JsonSerializationMixedContent) {
  Message msg = Message::user("Mixed content message");
  msg.attachments.push_back(ContentPart{
      .type = ContentType::Image,
      .data = {},
      .mime_type = "image/png",
      .url = "https://example.com/img.png",
      .is_base64 = false,
  });
  msg.attachments.push_back(ContentPart{
      .type = ContentType::Audio,
      .data = "audiobase64data",
      .mime_type = "audio/wav",
      .url = {},
      .is_base64 = true,
  });

  // Reproduce the OpenAI format logic from build_request_json
  Json content_arr = Json::array();
  if (!msg.content.empty()) {
    content_arr.push_back(Json{{"type", "text"}, {"text", msg.content}});
  }
  for (const auto &part : msg.attachments) {
    switch (part.type) {
    case ContentType::Image: {
      Json img_obj = Json::object();
      img_obj["type"] = "image_url";
      Json url_obj = Json::object();
      if (part.is_base64 && !part.data.empty()) {
        url_obj["url"] = fmt::format("data:{};base64,{}", part.mime_type, part.data);
      } else {
        url_obj["url"] = part.url;
      }
      img_obj["image_url"] = url_obj;
      content_arr.push_back(img_obj);
      break;
    }
    case ContentType::Audio: {
      Json audio_obj = Json::object();
      audio_obj["type"] = "input_audio";
      Json audio_data = Json::object();
      audio_data["data"] = part.data;
      audio_data["format"] = part.mime_type;
      audio_obj["input_audio"] = audio_data;
      content_arr.push_back(audio_obj);
      break;
    }
    default:
      break;
    }
  }

  ASSERT_EQ(content_arr.size(), 3u);
  EXPECT_EQ(content_arr[0]["type"], "text");
  EXPECT_EQ(content_arr[0]["text"], "Mixed content message");
  EXPECT_EQ(content_arr[1]["type"], "image_url");
  EXPECT_EQ(content_arr[1]["image_url"]["url"], "https://example.com/img.png");
  EXPECT_EQ(content_arr[2]["type"], "input_audio");
  EXPECT_EQ(content_arr[2]["input_audio"]["data"], "audiobase64data");
  EXPECT_EQ(content_arr[2]["input_audio"]["format"], "audio/wav");
}

// ─────────────────────────────────────────────────────────────
// 9. OpenAI format: verify image_url format for URL-based images
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, OpenAIFormatImageUrl) {
  ContentPart part{
      .type = ContentType::Image,
      .data = {},
      .mime_type = "image/jpeg",
      .url = "https://cdn.example.com/photo.jpg",
      .is_base64 = false,
  };

  // URL-based image: url field should be used directly
  Json img_obj = Json::object();
  img_obj["type"] = "image_url";
  Json url_obj = Json::object();
  if (part.is_base64 && !part.data.empty()) {
    url_obj["url"] = fmt::format("data:{};base64,{}", part.mime_type, part.data);
  } else {
    url_obj["url"] = part.url;
  }
  img_obj["image_url"] = url_obj;

  EXPECT_EQ(img_obj["type"], "image_url");
  EXPECT_EQ(img_obj["image_url"]["url"], "https://cdn.example.com/photo.jpg");
  // Must NOT contain "data:" prefix
  std::string url_str = img_obj["image_url"]["url"].get<std::string>();
  EXPECT_FALSE(url_str.starts_with("data:"));
}

// ─────────────────────────────────────────────────────────────
// 10. OpenAI format: verify data URI format for base64 images
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, OpenAIFormatBase64DataUri) {
  ContentPart part{
      .type = ContentType::Image,
      .data = "iVBORw0KGgoAAAANSUhEUg==",
      .mime_type = "image/png",
      .url = {},
      .is_base64 = true,
  };

  Json url_obj = Json::object();
  if (part.is_base64 && !part.data.empty()) {
    url_obj["url"] = fmt::format("data:{};base64,{}", part.mime_type, part.data);
  } else {
    url_obj["url"] = part.url;
  }

  std::string url_str = url_obj["url"].get<std::string>();
  EXPECT_TRUE(url_str.starts_with("data:image/png;base64,"));
  EXPECT_NE(url_str.find("iVBORw0KGgoAAAANSUhEUg=="), std::string::npos);
  EXPECT_EQ(url_str, "data:image/png;base64,iVBORw0KGgoAAAANSUhEUg==");
}

// ─────────────────────────────────────────────────────────────
// 11. Anthropic backend format for multimodal (simulated)
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, AnthropicStyleMultimodal) {
  // Anthropic uses a different format: source.type = "base64", source.media_type, source.data
  // Verify our ContentPart stores enough info to produce this format
  ContentPart part{
      .type = ContentType::Image,
      .data = "iVBORw0KGgo=",
      .mime_type = "image/png",
      .url = {},
      .is_base64 = true,
  };

  // Build Anthropic-style JSON from our ContentPart
  Json anthropic_content = Json::object();
  anthropic_content["type"] = "image";
  Json source = Json::object();
  source["type"] = "base64";
  source["media_type"] = part.mime_type;
  source["data"] = part.data;
  anthropic_content["source"] = source;

  EXPECT_EQ(anthropic_content["type"], "image");
  EXPECT_EQ(anthropic_content["source"]["type"], "base64");
  EXPECT_EQ(anthropic_content["source"]["media_type"], "image/png");
  EXPECT_EQ(anthropic_content["source"]["data"], "iVBORw0KGgo=");

  // Also verify URL-based Anthropic format
  ContentPart url_part{
      .type = ContentType::Image,
      .data = {},
      .mime_type = "image/jpeg",
      .url = "https://example.com/photo.jpg",
      .is_base64 = false,
  };

  Json anthropic_url = Json::object();
  anthropic_url["type"] = "image";
  Json url_source = Json::object();
  url_source["type"] = "url";
  url_source["url"] = url_part.url;
  anthropic_url["source"] = url_source;

  EXPECT_EQ(anthropic_url["source"]["type"], "url");
  EXPECT_EQ(anthropic_url["source"]["url"], "https://example.com/photo.jpg");
}

// ─────────────────────────────────────────────────────────────
// 12. Edge case: attachment with empty data
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, AttachmentWithEmptyData) {
  ContentPart part{
      .type = ContentType::Image,
      .data = {},
      .mime_type = "image/png",
      .url = {},
      .is_base64 = false,
  };
  EXPECT_TRUE(part.data.empty());
  EXPECT_TRUE(part.url.empty());

  // A message with an empty attachment should not crash MockBackend
  Message msg = Message::user("Test");
  msg.attachments.push_back(part);

  auto req = LLMRequest::builder()
                 .add_message(std::move(msg))
                 .build();

  MockLLMBackend mock;
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->finish_reason, "stop");
}

// ─────────────────────────────────────────────────────────────
// 13. Edge case: attachment with very long URL
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, AttachmentWithVeryLongUrl) {
  std::string long_url = "https://example.com/images/";
  long_url += std::string(8000, 'a'); // 8000+ char URL
  long_url += ".png";

  auto msg = Message::user_with_image("Describe", long_url);
  ASSERT_EQ(msg.attachments.size(), 1u);
  EXPECT_EQ(msg.attachments[0].url, long_url);
  EXPECT_GT(msg.attachments[0].url.size(), 8000u);

  // Should not crash when used in a request
  auto req = LLMRequest::builder()
                 .add_message(std::move(msg))
                 .build();

  MockLLMBackend mock;
  auto result = mock.complete(req);
  ASSERT_TRUE(result);
}

// ─────────────────────────────────────────────────────────────
// 14. ContentType enum completeness
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, ContentTypeEnumCompleteness) {
  // All four ContentType values should be distinct
  std::vector<int> values = {
      static_cast<int>(ContentType::Text),
      static_cast<int>(ContentType::Image),
      static_cast<int>(ContentType::Audio),
      static_cast<int>(ContentType::Video),
  };

  // Check uniqueness
  for (size_t i = 0; i < values.size(); ++i) {
    for (size_t j = i + 1; j < values.size(); ++j) {
      EXPECT_NE(values[i], values[j])
          << "ContentType values at index " << i << " and " << j << " collide";
    }
  }

  // Verify we can switch over all values without warnings
  auto describe = [](ContentType ct) -> std::string {
    switch (ct) {
    case ContentType::Text:
      return "text";
    case ContentType::Image:
      return "image";
    case ContentType::Audio:
      return "audio";
    case ContentType::Video:
      return "video";
    }
    return "unknown";
  };

  EXPECT_EQ(describe(ContentType::Text), "text");
  EXPECT_EQ(describe(ContentType::Image), "image");
  EXPECT_EQ(describe(ContentType::Audio), "audio");
  EXPECT_EQ(describe(ContentType::Video), "video");
}

// ─────────────────────────────────────────────────────────────
// 15. Copy/move semantics of Message with attachments
// ─────────────────────────────────────────────────────────────

TEST(MultiModalExtended, MessageCopySemantics) {
  auto original = Message::user_with_image("Describe", "https://example.com/img.png");
  original.attachments.push_back(ContentPart{
      .type = ContentType::Audio,
      .data = "audiodata",
      .mime_type = "audio/wav",
      .url = {},
      .is_base64 = true,
  });

  // Copy
  Message copy = original;
  EXPECT_EQ(copy.role, original.role);
  EXPECT_EQ(copy.content, original.content);
  ASSERT_EQ(copy.attachments.size(), original.attachments.size());
  EXPECT_EQ(copy.attachments[0].url, original.attachments[0].url);
  EXPECT_EQ(copy.attachments[1].data, original.attachments[1].data);

  // Modify copy should not affect original
  copy.content = "Modified";
  copy.attachments[0].url = "https://other.com/img.png";
  EXPECT_EQ(original.content, "Describe");
  EXPECT_EQ(original.attachments[0].url, "https://example.com/img.png");
}

TEST(MultiModalExtended, MessageMoveSemantics) {
  auto original = Message::user_with_image("Describe", "https://example.com/img.png");
  original.attachments.push_back(ContentPart{
      .type = ContentType::Audio,
      .data = "audiodata",
      .mime_type = "audio/wav",
      .url = {},
      .is_base64 = true,
  });

  std::string original_content = original.content;
  size_t original_attachment_count = original.attachments.size();

  // Move
  Message moved = std::move(original);
  EXPECT_EQ(moved.role, Role::User);
  EXPECT_EQ(moved.content, original_content);
  ASSERT_EQ(moved.attachments.size(), original_attachment_count);
  EXPECT_EQ(moved.attachments[0].type, ContentType::Image);
  EXPECT_EQ(moved.attachments[1].type, ContentType::Audio);
}

TEST(MultiModalExtended, MessageMoveAssignment) {
  auto msg1 = Message::user_with_image("First", "https://example.com/1.png");
  auto msg2 = Message::user("Second");

  msg2 = std::move(msg1);
  EXPECT_EQ(msg2.content, "First");
  ASSERT_EQ(msg2.attachments.size(), 1u);
  EXPECT_EQ(msg2.attachments[0].url, "https://example.com/1.png");
}
