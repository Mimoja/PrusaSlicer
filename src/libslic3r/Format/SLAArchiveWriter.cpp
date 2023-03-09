#include "SLAArchiveWriter.hpp"

#include "SL1.hpp"
#include "SL1_SVG.hpp"
#include "AnycubicSLA.hpp"
#include "CTB.hpp"

#include "libslic3r/libslic3r.h"

#include <string>
#include <map>
#include <memory>
#include <tuple>

namespace Slic3r {

using ArchiveFactory = std::function<std::unique_ptr<SLAArchiveWriter>(const SLAPrinterConfig&)>;

struct ArchiveEntry {
    const char *ext;
    ArchiveFactory factoryfn;
};

static const std::map<std::string, ArchiveEntry> REGISTERED_ARCHIVES {
    {
        "SL1",
        { "sl1",  [] (const auto &cfg) { return std::make_unique<SL1Archive>(cfg); } }
    },
    {
        "SL2",
        { "sl2",  [] (const auto &cfg) { return std::make_unique<SL1_SVGArchive>(cfg); } }
    },
    // Supports only ANYCUBIC_SLA_VERSION_1
    ANYCUBIC_SLA_FORMAT_VERSIONED("pws", "Photon / Photon S",   ANYCUBIC_SLA_VERSION_1),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pw0", "Photon Zero",         ANYCUBIC_SLA_VERSION_1),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pwx", "Photon X",            ANYCUBIC_SLA_VERSION_1),

    // Supports ANYCUBIC_SLA_VERSION_1 and ANYCUBIC_SLA_VERSION_515
    // 515 only brings greyscale correction data which we do not benefit from
    ANYCUBIC_SLA_FORMAT_VERSIONED("pwmo", "Photon Mono",        ANYCUBIC_SLA_VERSION_1),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pwms", "Photon Mono SE",     ANYCUBIC_SLA_VERSION_1),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pwmx", "Photon Mono X",      ANYCUBIC_SLA_VERSION_1),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pmsq", "Photon Mono SQ",     ANYCUBIC_SLA_VERSION_1),
    ANYCUBIC_SLA_FORMAT_VERSIONED("dlp",  "Photon Ultra",       ANYCUBIC_SLA_VERSION_1),

    // Supports ANYCUBIC_SLA_VERSION_515 and ANYCUBIC_SLA_VERSION_516
    // v516 offers additional parameters we are using
    ANYCUBIC_SLA_FORMAT_VERSIONED("pwma",  "Photon Mono 4K",    ANYCUBIC_SLA_VERSION_516),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pm3",   "Photon M3",         ANYCUBIC_SLA_VERSION_516),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pm3m",  "Photon M3 Max",     ANYCUBIC_SLA_VERSION_516),

    // Supports ANYCUBIC_SLA_VERSION_515 to ANYCUBIC_SLA_VERSION_517
    // v517 offers no additional benefit to us unless we are debugging the output file in PhotonWorkshop
    ANYCUBIC_SLA_FORMAT_VERSIONED("pwmb",  "Photon Ultra",       ANYCUBIC_SLA_VERSION_516),
    ANYCUBIC_SLA_FORMAT_VERSIONED("dl2p",  "Photon Ultra",       ANYCUBIC_SLA_VERSION_516),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pmx2",  "Photon Ultra",       ANYCUBIC_SLA_VERSION_516),
    ANYCUBIC_SLA_FORMAT_VERSIONED("pm3r",  "Photon M3 Premium",  ANYCUBIC_SLA_VERSION_516),

    {
        "ctb",
        { "ctb",  [] (const auto &cfg) { return std::make_unique<CTBArchive>(cfg); } }
    },
};

std::unique_ptr<SLAArchiveWriter>
SLAArchiveWriter::create(const std::string &archtype, const SLAPrinterConfig &cfg)
{
    auto entry = REGISTERED_ARCHIVES.find(archtype);

    if (entry != REGISTERED_ARCHIVES.end())
        return entry->second.factoryfn(cfg);

    return nullptr;
}

const std::vector<const char*>& SLAArchiveWriter::registered_archives()
{
    static std::vector<const char*> archnames;

    if (archnames.empty()) {
        archnames.reserve(REGISTERED_ARCHIVES.size());

        for (auto &[name, _] : REGISTERED_ARCHIVES)
            archnames.emplace_back(name.c_str());
    }

    return archnames;
}

const char *SLAArchiveWriter::get_extension(const char *archtype)
{
    constexpr const char* DEFAULT_EXT = "zip";

    auto entry = REGISTERED_ARCHIVES.find(archtype);
    if (entry != REGISTERED_ARCHIVES.end())
        return entry->second.ext;

    return DEFAULT_EXT;
}

} // namespace Slic3r
