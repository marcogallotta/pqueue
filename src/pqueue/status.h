#pragma once

namespace pqueue {

enum class StatusCode {
    Ok,

    InvalidArgument,
    BackendUnavailable,

    MountFailed,
    ReadFailed,
    WriteFailed,
    RenameFailed,
    RemoveFailed,
    ListFailed,

    InvalidIndex,
    InvalidRecord,
    CrcMismatch,
    RecordTooLarge,
    QueueFull,
    QueueEmpty,
    LockTimeout,

    EncodeFailed,
    DecodeFailed,
    SendFailed,
    Dropped,

    DataCorrupt,
};

inline const char* statusCodeName(StatusCode code) {
    switch (code) {
        case StatusCode::Ok: return "ok";
        case StatusCode::InvalidArgument: return "invalid_argument";
        case StatusCode::BackendUnavailable: return "backend_unavailable";
        case StatusCode::MountFailed: return "mount_failed";
        case StatusCode::ReadFailed: return "read_failed";
        case StatusCode::WriteFailed: return "write_failed";
        case StatusCode::RenameFailed: return "rename_failed";
        case StatusCode::RemoveFailed: return "remove_failed";
        case StatusCode::ListFailed: return "list_failed";
        case StatusCode::InvalidIndex: return "invalid_index";
        case StatusCode::InvalidRecord: return "invalid_record";
        case StatusCode::CrcMismatch: return "crc_mismatch";
        case StatusCode::RecordTooLarge: return "record_too_large";
        case StatusCode::QueueFull: return "queue_full";
        case StatusCode::QueueEmpty: return "queue_empty";
        case StatusCode::LockTimeout: return "lock_timeout";
        case StatusCode::EncodeFailed: return "encode_failed";
        case StatusCode::DecodeFailed: return "decode_failed";
        case StatusCode::SendFailed: return "send_failed";
        case StatusCode::Dropped: return "dropped";
        case StatusCode::DataCorrupt: return "data_corrupt";
    }
    return "unknown";
}

struct Status {
    StatusCode code = StatusCode::Ok;
    int backendCode = 0;
    const char* message = "";

    bool ok() const { return code == StatusCode::Ok; }

    static Status success() { return {}; }
    static Status failure(StatusCode code, const char* message, int backendCode = 0) {
        return {code, backendCode, message};
    }
};

} // namespace pqueue
