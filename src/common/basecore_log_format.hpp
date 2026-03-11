#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "logging/log_format_registry.hpp"

namespace acct_service::basecore_log_adapter {

using FileField = std::array<char, 96>;
using SeverityField = std::array<char, 16>;
using CodeField = std::array<char, 32>;
using DomainField = std::array<char, 16>;
using MessageField = std::array<char, 256>;

// Formats account_services log records during LogReader decode.
class ProjectLogFormat {
public:
    static constexpr uint8_t kRecordLogId = 64;
    static constexpr uint16_t kRecordLogPayloadSize =
        static_cast<uint16_t>(sizeof(FileField) + sizeof(uint32_t) + sizeof(SeverityField) + sizeof(CodeField) +
                              sizeof(DomainField) + sizeof(int) + sizeof(MessageField));

    // Renders the common account_services log payload into a readable line body.
    static std::string record_log(const FileField& file, uint32_t line, const SeverityField& severity,
                                  const CodeField& code, const DomainField& domain, int sys_errno,
                                  const MessageField& message);

    // Decodes the fixed-size payload layout used by account_services log records.
    static void decode_record_log(const uint8_t* payload, uint16_t size, char* buffer, std::size_t buffer_size);

    // Registers account_services-specific format decoders into BaseCore.
    static bool register_formats() noexcept;
};

}  // namespace acct_service::basecore_log_adapter
