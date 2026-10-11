// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jurgen Kobierczynski
//
// DNG camera profiles (.dcp files): the description of a camera's colour that
// raw developers choose between ("Adobe Standard", "Camera Portrait", a
// profile made from a colour chart ...). A profile can be stored inside a
// DNG, where it travels with the picture.
//
// Nothing in a profile is interpreted here beyond what is needed to tell
// whether it is sound and which camera it is for: the tables are carried
// from the .dcp file into the DNG value for value.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "md5.hpp"
#include "raw_image.hpp"

namespace dngconv {

struct CameraProfile {
    std::string name;         // ProfileName; may be empty
    std::string cameraModel;  // the camera the profile was made for; empty = any
    std::string copyright;
    std::string groupName;    // ProfileGroupName (DNG 1.7); usually empty
    // ProfileEmbedPolicy: 0 = allow copying, 1 = embed if used,
    // 2 = never embed, 3 = no restrictions.
    uint32_t embedPolicy = 0;
    unsigned colorPlanes = 3;
    unsigned illuminants = 1;  // calibrations the profile interpolates between
    bool hasHueSatMap = false;
    bool hasLookTable = false;
    bool hasToneCurve = false;
    bool hasForwardMatrix = false;
    // The DNG version that introduced the newest tag in the profile, as
    // major * 256 + minor: 0x0104 unless the profile uses later additions.
    unsigned dngVersion = 0x0104;

    // The profile's tags, values little-endian, sorted by tag. The camera
    // model is not among them: inside a DNG the file's own model applies.
    std::vector<TiffField> fields;
    // Fingerprint of `fields`: equal for the same profile from two files.
    Md5Digest digest{};

    std::string fileName;  // where it was read from, for messages

    const TiffField* find(uint16_t tag) const;
    /// "Adobe Standard" or, for a profile without a name, its file name.
    std::string displayName() const;
    /// "two illuminants, hue and saturation table, look table, tone curve".
    std::string summary() const;
    bool mayBeEmbedded() const { return embedPolicy != 2; }
};

/// Parses a camera profile held in memory. Throws std::runtime_error saying
/// what is wrong when the data is not a usable profile.
CameraProfile parseCameraProfile(const uint8_t* data, size_t size);

/// Reads a .dcp file. Errors as above, with the file named.
CameraProfile readCameraProfile(const std::filesystem::path& file);

/// The profile as a self-contained block for the DNG ExtraCameraProfiles tag.
std::vector<uint8_t> packCameraProfile(const CameraProfile& profile, bool bigEndian);

/// Whether two camera model names mean the same camera: letters and digits
/// are compared, case, spaces and punctuation are not.
bool sameCameraModel(const std::string& a, const std::string& b);

/// A set of profiles to draw on, in the order they were given.
class ProfileLibrary {
public:
    /// Adds a .dcp file, or every .dcp file in a folder and below it (sorted
    /// by path). An unusable file named directly throws; one found in a
    /// folder is skipped and listed in `skipped` ("file: reason").
    void add(const std::filesystem::path& fileOrFolder, std::vector<std::string>& skipped);

    bool empty() const { return profiles_.empty(); }
    size_t size() const { return profiles_.size(); }
    const std::vector<std::shared_ptr<const CameraProfile>>& all() const { return profiles_; }

    /// The profiles that fit a camera, first given first: same model (or no
    /// model named; any model with `anyCamera`), same number of colour planes,
    /// allowed to be embedded. A second profile with a name already taken is
    /// left out. `notes` receives what was left out and why, where that may
    /// surprise.
    std::vector<std::shared_ptr<const CameraProfile>> select(const std::string& cameraModel,
                                                             unsigned colorPlanes, bool anyCamera,
                                                             std::vector<std::string>& notes) const;

private:
    std::vector<std::shared_ptr<const CameraProfile>> profiles_;
};

}  // namespace dngconv
