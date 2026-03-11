#include "common/basecore_log_format.hpp"

#include <cstdio>
#include <cstring>

namespace acct_service::basecore_log_adapter {

namespace {

// Forces registration into every binary that links this translation unit.
[[maybe_unused]] const bool kRegistered = ProjectLogFormat::register_formats();

}  // namespace

// Renders the common account_services log payload into a readable line body.
std::string ProjectLogFormat::record_log(const FileField& file, uint32_t line, const SeverityField& severity,
                                         const CodeField& code, const DomainField& domain, int sys_errno,
                                         const MessageField& message) {
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer), "[%s:%u] severity=%s code=%s domain=%s errno=%d msg=%s", file.data(), line,
                  severity.data(), code.data(), domain.data(), sys_errno, message.data());
    return std::string(buffer);
}

// Decodes the fixed-size payload layout used by account_services log records.
void ProjectLogFormat::decode_record_log(const uint8_t* payload, uint16_t size, char* buffer, std::size_t buffer_size) {
    if (!payload || !buffer || buffer_size == 0 || size != kRecordLogPayloadSize) {
        return;
    }

    std::size_t offset = 0;
    FileField file{};
    SeverityField severity{};
    CodeField code{};
    DomainField domain{};
    MessageField message{};
    uint32_t line = 0;
    int sys_errno = 0;

    std::memcpy(file.data(), payload + offset, sizeof(file));
    offset += sizeof(file);
    std::memcpy(&line, payload + offset, sizeof(line));
    offset += sizeof(line);
    std::memcpy(severity.data(), payload + offset, sizeof(severity));
    offset += sizeof(severity);
    std::memcpy(code.data(), payload + offset, sizeof(code));
    offset += sizeof(code);
    std::memcpy(domain.data(), payload + offset, sizeof(domain));
    offset += sizeof(domain);
    std::memcpy(&sys_errno, payload + offset, sizeof(sys_errno));
    offset += sizeof(sys_errno);
    std::memcpy(message.data(), payload + offset, sizeof(message));

    const std::string rendered = record_log(file, line, severity, code, domain, sys_errno, message);
    std::snprintf(buffer, buffer_size, "%s", rendered.c_str());
}

// Registers account_services-specific format decoders into BaseCore.
bool ProjectLogFormat::register_formats() noexcept {
    return base_core_log::LogFormatRegistry::register_func(kRecordLogId, &ProjectLogFormat::decode_record_log,
                                                           kRecordLogPayloadSize);
}

}  // namespace acct_service::basecore_log_adapter
