#pragma once

// The disc's directory table ("FST") as a TEST ORACLE. Production code knows
// nothing of this format: the shipping namespace is built from the host tree
// (gekko/asset_namespace.h) and nothing is published into guest memory. The
// tests keep the format here, for exactly two purposes:
//
//  * asset_namespace_tests prove the host tree reproduces the disc's table
//    entry for entry (numbering, names, nesting, sizes, name layout);
//  * the reference tests hand the native file API the very tables the
//    ORIGINAL SDK words walk, and publish them into the interpreter's guest
//    memory for those words to read.
//
// Table layout: 12-byte records in entry order, then the NUL-terminated
// names. word0 = directory flag << 24 | name offset; word1 = parent
// (directory) or a file locator; word2 = subtree end (directory) or size.

#include <string>
#include <vector>

#include "gekko/asset_mount.h"
#include "gekko/asset_namespace.h"
#include "gekko/types.h"

namespace gekko::test_support {

// Decodes `table` exactly in table order (nothing reordered) and builds the
// namespace from it. A directory whose word1 disagrees with its nesting, a
// root that is not a directory or names a parent, an overrun, or a bad name is
// refused.
bool namespace_from_fst(const std::vector<u8>& table, assets::AssetNamespace& out, std::string& error);

// The table the boot used to publish below BI2 for a mounted asset system:
// the namespace in the layout above, every file's word1 being its guest token.
std::vector<u8> fst_publication(const assets::AssetSystem& assets);

}  // namespace gekko::test_support
