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
