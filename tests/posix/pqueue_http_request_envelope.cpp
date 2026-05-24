#include "pqueue/http/request_envelope.h"

#include "doctest/doctest.h"

#include <string>

TEST_CASE("pqueue http request envelope round trips post request") {
    pqueue::http::RequestEnvelope request;
    request.method = pqueue::http::Method::Post;
    request.path = "/switchbot/reading";
    request.body = "{\"ok\":true}";

    std::string encoded;
    REQUIRE(pqueue::http::encodeRequestEnvelope(request, encoded));

    pqueue::http::RequestEnvelope decoded;
    REQUIRE(pqueue::http::decodeRequestEnvelope(encoded, decoded));

    CHECK(decoded.method == pqueue::http::Method::Post);
    CHECK_EQ(decoded.path, request.path);
    CHECK_EQ(decoded.body, request.body);
}

TEST_CASE("pqueue http request envelope round trips binary body") {
    pqueue::http::RequestEnvelope request;
    request.method = pqueue::http::Method::Post;
    request.path = "/binary";
    request.body.push_back('a');
    request.body.push_back('\0');
    request.body.push_back(static_cast<char>(0xff));
    request.body.push_back('z');

    std::string encoded;
    REQUIRE(pqueue::http::encodeRequestEnvelope(request, encoded));

    pqueue::http::RequestEnvelope decoded;
    REQUIRE(pqueue::http::decodeRequestEnvelope(encoded, decoded));

    CHECK_EQ(decoded.path, request.path);
    CHECK_EQ(decoded.body, request.body);
}

TEST_CASE("pqueue http request envelope rejects truncated records") {
    pqueue::http::RequestEnvelope request;
    request.method = pqueue::http::Method::Post;
    request.path = "/path";
    request.body = "body";

    std::string encoded;
    REQUIRE(pqueue::http::encodeRequestEnvelope(request, encoded));
    encoded.resize(encoded.size() - 1);

    pqueue::http::RequestEnvelope decoded;
    CHECK_FALSE(pqueue::http::decodeRequestEnvelope(encoded, decoded));
}

TEST_CASE("pqueue http request envelope v1 record decodes as Raw encoding") {
    // Manually construct a v1 envelope: magic(4) + version=1(1) + method=POST(1) + path_len(2) + body_len(4) + path + body
    const std::string path = "/switchbot/reading";
    const std::string body = "{\"ok\":true}";
    std::string v1;
    // magic 0x50514852 little-endian
    v1.push_back('\x52'); v1.push_back('\x48'); v1.push_back('\x51'); v1.push_back('\x50');
    v1.push_back('\x01'); // version 1
    v1.push_back('\x01'); // method POST
    const std::uint16_t pathLen = static_cast<std::uint16_t>(path.size());
    v1.push_back(static_cast<char>(pathLen & 0xff));
    v1.push_back(static_cast<char>((pathLen >> 8) & 0xff));
    const std::uint32_t bodyLen = static_cast<std::uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) {
        v1.push_back(static_cast<char>((bodyLen >> (8 * i)) & 0xff));
    }
    v1.append(path);
    v1.append(body);

    pqueue::http::RequestEnvelope decoded;
    REQUIRE(pqueue::http::decodeRequestEnvelope(v1, decoded));
    CHECK(decoded.encoding == pqueue::http::BodyEncoding::Raw);
    CHECK_EQ(decoded.method, pqueue::http::Method::Post);
    CHECK_EQ(decoded.path, path);
    CHECK_EQ(decoded.body, body);
}

TEST_CASE("pqueue http request envelope v2 Raw round trips") {
    pqueue::http::RequestEnvelope request;
    request.method = pqueue::http::Method::Post;
    request.encoding = pqueue::http::BodyEncoding::Raw;
    request.path = "/switchbot/reading";
    request.body = "{\"ok\":true}";

    std::string encoded;
    REQUIRE(pqueue::http::encodeRequestEnvelope(request, encoded));

    pqueue::http::RequestEnvelope decoded;
    REQUIRE(pqueue::http::decodeRequestEnvelope(encoded, decoded));

    CHECK(decoded.encoding == pqueue::http::BodyEncoding::Raw);
    CHECK_EQ(decoded.path, request.path);
    CHECK_EQ(decoded.body, request.body);
}

TEST_CASE("pqueue http request envelope v2 Compact round trips") {
    pqueue::http::RequestEnvelope request;
    request.method = pqueue::http::Method::Post;
    request.encoding = pqueue::http::BodyEncoding::Compact;
    request.path = "/switchbot/reading";
    request.body.push_back('\x01'); // version
    request.body.push_back('\x01'); // type switchbot
    request.body.push_back('\xAA'); // mac byte

    std::string encoded;
    REQUIRE(pqueue::http::encodeRequestEnvelope(request, encoded));

    pqueue::http::RequestEnvelope decoded;
    REQUIRE(pqueue::http::decodeRequestEnvelope(encoded, decoded));

    CHECK(decoded.encoding == pqueue::http::BodyEncoding::Compact);
    CHECK_EQ(decoded.path, request.path);
    CHECK_EQ(decoded.body, request.body);
}

TEST_CASE("pqueue http request envelope rejects unknown encoding byte") {
    pqueue::http::RequestEnvelope request;
    request.method = pqueue::http::Method::Post;
    request.encoding = pqueue::http::BodyEncoding::Raw;
    request.path = "/path";
    request.body = "body";

    std::string encoded;
    REQUIRE(pqueue::http::encodeRequestEnvelope(request, encoded));

    // Encoding byte is at offset 6 (magic=4, version=1, method=1). Corrupt it.
    encoded[6] = static_cast<char>(0xff);

    pqueue::http::RequestEnvelope decoded;
    CHECK_FALSE(pqueue::http::decodeRequestEnvelope(encoded, decoded));
}

TEST_CASE("pqueue http request envelope rejects unknown version") {
    pqueue::http::RequestEnvelope request;
    request.method = pqueue::http::Method::Post;
    request.path = "/path";
    request.body = "body";

    std::string encoded;
    REQUIRE(pqueue::http::encodeRequestEnvelope(request, encoded));

    // Version byte is at offset 4. Set to unknown version 3.
    encoded[4] = static_cast<char>(0x03);

    pqueue::http::RequestEnvelope decoded;
    CHECK_FALSE(pqueue::http::decodeRequestEnvelope(encoded, decoded));
}

TEST_CASE("pqueue http request envelope encode rejects invalid BodyEncoding") {
    pqueue::http::RequestEnvelope request;
    request.method = pqueue::http::Method::Post;
    request.path = "/path";
    request.body = "body";
    request.encoding = static_cast<pqueue::http::BodyEncoding>(0xff);

    std::string encoded;
    CHECK_FALSE(pqueue::http::encodeRequestEnvelope(request, encoded));
}
