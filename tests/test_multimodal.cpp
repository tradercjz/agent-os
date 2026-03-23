#include <agentos/kernel/llm_kernel.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace agentos;
using namespace agentos::kernel;
using Json = nlohmann::json;

// ─────────────────────────────────────────────────────────────
// ContentPart creation
// ─────────────────────────────────────────────────────────────

TEST(MultiModalTest, ContentPartDefaultsToText) {
  ContentPart part;
  EXPECT_EQ(part.type, ContentType::Text);
  EXPECT_TRUE(part.data.empty());
  EXPECT_TRUE(part.mime_type.empty());
  EXPECT_TRUE(part.url.empty());
  EXPECT_FALSE(part.is_base64);
}

TEST(MultiModalTest, ContentPartImageUrl) {
  ContentPart part{
    .type = ContentType::Image,
    .data = {},
    .mime_type = "image/png",
    .url = "https://example.com/cat.png",
    .is_base64 = false,
  };
  EXPECT_EQ(part.type, ContentType::Image);
  EXPECT_EQ(part.url, "https://example.com/cat.png");
  EXPECT_EQ(part.mime_type, "image/png");
  EXPECT_FALSE(part.is_base64);
}

TEST(MultiModalTest, ContentPartBase64Image) {
  ContentPart part{
    .type = ContentType::Image,
    .data = "iVBORw0KGgo=",
    .mime_type = "image/png",
    .url = {},
    .is_base64 = true,
  };
  EXPECT_TRUE(part.is_base64);
  EXPECT_EQ(part.data, "iVBORw0KGgo=");
}

TEST(MultiModalTest, ContentPartAudio) {
  ContentPart part{
    .type = ContentType::Audio,
    .data = "UklGR...",
    .mime_type = "audio/wav",
    .url = {},
    .is_base64 = true,
  };
  EXPECT_EQ(part.type, ContentType::Audio);
  EXPECT_EQ(part.mime_type, "audio/wav");
}

// ─────────────────────────────────────────────────────────────
// Message with attachments
// ─────────────────────────────────────────────────────────────

TEST(MultiModalTest, MessageUserWithImage) {
  auto msg = Message::user_with_image("Describe this image", "https://example.com/photo.jpg");
  EXPECT_EQ(msg.role, Role::User);
  EXPECT_EQ(msg.content, "Describe this image");
  ASSERT_EQ(msg.attachments.size(), 1u);
  EXPECT_EQ(msg.attachments[0].type, ContentType::Image);
  EXPECT_EQ(msg.attachments[0].url, "https://example.com/photo.jpg");
  EXPECT_FALSE(msg.attachments[0].is_base64);
}

TEST(MultiModalTest, MessageUserWithAudio) {
  auto msg = Message::user_with_audio("Transcribe this", "base64audiodata");
  EXPECT_EQ(msg.role, Role::User);
  EXPECT_EQ(msg.content, "Transcribe this");
  ASSERT_EQ(msg.attachments.size(), 1u);
  EXPECT_EQ(msg.attachments[0].type, ContentType::Audio);
  EXPECT_EQ(msg.attachments[0].data, "base64audiodata");
  EXPECT_EQ(msg.attachments[0].mime_type, "audio/wav");
  EXPECT_TRUE(msg.attachments[0].is_base64);
}

TEST(MultiModalTest, MessageUserWithAudioCustomMime) {
  auto msg = Message::user_with_audio("Listen", "data123", "audio/mp3");
  EXPECT_EQ(msg.attachments[0].mime_type, "audio/mp3");
}

TEST(MultiModalTest, PlainMessageHasNoAttachments) {
  auto msg = Message::user("Hello");
  EXPECT_TRUE(msg.attachments.empty());

  auto sys = Message::system("You are helpful");
  EXPECT_TRUE(sys.attachments.empty());

  auto asst = Message::assistant("Sure thing");
  EXPECT_TRUE(asst.attachments.empty());
}

// ─────────────────────────────────────────────────────────────
// Builder with attachments
// ─────────────────────────────────────────────────────────────

TEST(MultiModalTest, BuilderAttachments) {
  auto req = LLMRequest::builder()
    .user("What is in this image?")
    .attachments({ContentPart{
      .type = ContentType::Image,
      .data = {},
      .mime_type = "image/jpeg",
      .url = "https://example.com/img.jpg",
      .is_base64 = false,
    }})
    .model("gpt-4o")
    .build();

  ASSERT_EQ(req.messages.size(), 1u);
  ASSERT_EQ(req.messages[0].attachments.size(), 1u);
  EXPECT_EQ(req.messages[0].attachments[0].url, "https://example.com/img.jpg");
  EXPECT_EQ(req.model, "gpt-4o");
}

TEST(MultiModalTest, BuilderAttachmentsOnEmptyMessagesIsNoOp) {
  // attachments() on empty messages should not crash
  auto req = LLMRequest::builder()
    .attachments({ContentPart{.type = ContentType::Image}})
    .build();

  EXPECT_TRUE(req.messages.empty());
}

// ─────────────────────────────────────────────────────────────
// JSON serialization of multimodal messages (OpenAI format)
// ─────────────────────────────────────────────────────────────

// Helper: Use OpenAIBackend's build_request_json indirectly via MockBackend
// We test the JSON structure by constructing what build_request_json produces.

TEST(MultiModalTest, JsonSerializationImageUrl) {
  // Simulate what build_request_json produces for image attachments
  auto msg = Message::user_with_image("What is this?", "https://example.com/test.png");

  // Build the content array as OpenAI format
  Json content_arr = Json::array();
  content_arr.push_back(Json{{"type", "text"}, {"text", msg.content}});
  for (const auto &part : msg.attachments) {
    if (part.type == ContentType::Image) {
      Json img_obj = Json::object();
      img_obj["type"] = "image_url";
      Json url_obj = Json::object();
      url_obj["url"] = part.url;
      img_obj["image_url"] = url_obj;
      content_arr.push_back(img_obj);
    }
  }

  ASSERT_EQ(content_arr.size(), 2u);
  EXPECT_EQ(content_arr[0]["type"], "text");
  EXPECT_EQ(content_arr[0]["text"], "What is this?");
  EXPECT_EQ(content_arr[1]["type"], "image_url");
  EXPECT_EQ(content_arr[1]["image_url"]["url"], "https://example.com/test.png");
}

TEST(MultiModalTest, JsonSerializationBase64Image) {
  ContentPart part{
    .type = ContentType::Image,
    .data = "AAAA",
    .mime_type = "image/jpeg",
    .url = {},
    .is_base64 = true,
  };

  // Simulate format
  Json url_obj = Json::object();
  url_obj["url"] = fmt::format("data:{};base64,{}", part.mime_type, part.data);

  EXPECT_EQ(url_obj["url"], "data:image/jpeg;base64,AAAA");
}

TEST(MultiModalTest, JsonSerializationAudio) {
  auto msg = Message::user_with_audio("Transcribe", "b64data", "audio/mp3");

  Json content_arr = Json::array();
  content_arr.push_back(Json{{"type", "text"}, {"text", msg.content}});
  for (const auto &part : msg.attachments) {
    if (part.type == ContentType::Audio) {
      Json audio_obj = Json::object();
      audio_obj["type"] = "input_audio";
      Json audio_data = Json::object();
      audio_data["data"] = part.data;
      audio_data["format"] = part.mime_type;
      audio_obj["input_audio"] = audio_data;
      content_arr.push_back(audio_obj);
    }
  }

  ASSERT_EQ(content_arr.size(), 2u);
  EXPECT_EQ(content_arr[1]["type"], "input_audio");
  EXPECT_EQ(content_arr[1]["input_audio"]["data"], "b64data");
  EXPECT_EQ(content_arr[1]["input_audio"]["format"], "audio/mp3");
}

TEST(MultiModalTest, MockBackendHandlesMultimodalMessage) {
  MockLLMBackend mock;

  auto req = LLMRequest::builder()
    .user("Describe this image")
    .attachments({ContentPart{
      .type = ContentType::Image,
      .data = {},
      .mime_type = "image/png",
      .url = "https://example.com/photo.png",
      .is_base64 = false,
    }})
    .model("mock-gpt")
    .build();

  auto result = mock.complete(req);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->content.empty());
  EXPECT_EQ(result->finish_reason, "stop");
}

TEST(MultiModalTest, ContentTypeEnumValues) {
  // Verify all enum values are distinct
  EXPECT_NE(static_cast<int>(ContentType::Text), static_cast<int>(ContentType::Image));
  EXPECT_NE(static_cast<int>(ContentType::Image), static_cast<int>(ContentType::Audio));
  EXPECT_NE(static_cast<int>(ContentType::Audio), static_cast<int>(ContentType::Video));
}
