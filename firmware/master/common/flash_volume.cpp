/* -*- mode: C; c-basic-offset: 4; intent-tabs-mode: nil -*-
 *
 * Thundercracker firmware
 *
 * Copyright <c> 2012 Sifteo, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "flash_map.h"
#include "flash_volume.h"
#include "flash_volumeheader.h"
#include "flash_recycler.h"
#include "flash_lfs.h"
#include "flash_syslfs.h"
#include "crc.h"
#include "elfprogram.h"
#include "event.h"
#include "tasks.h"

bool FlashVolume::isValid() const
{
    ASSERT(this);
    if (!block.isValid())
        return false;

    FlashBlockRef hdrRef;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(hdrRef, block);

    if (!hdr->isHeaderValid())
        return false;

    /*
     * Check CRCs. Note that the Map is not checked if this is
     * a deleted volume, since we selectively invalidate map entries
     * as they are recycled.
     */

    unsigned numMapEntries = hdr->numMapEntries();
    if (!FlashVolume::typeIsRecyclable(hdr->type) && hdr->crcMap != hdr->calculateMapCRC(numMapEntries))
        return false;
    if (hdr->crcErase != hdr->calculateEraseCountCRC(block, numMapEntries))
        return false;

    /*
     * Map assertions: These aren't necessary on release builds, we
     * just want to check over some of our layout assumptions during
     * testing on simulator builds.
     */

    DEBUG_ONLY({
        const FlashMap* map = hdr->getMap();

        // First map entry always describes the volume header
        ASSERT(numMapEntries >= 1);
        ASSERT(map->blocks[0].code == block.code);

        // LFS volumes always have numMapEntries==1
        ASSERT(numMapEntries == 1 || hdr->type != T_LFS);

        /*
         * Header must have the lowest block index, otherwise FlashVolumeIter
         * might find a non-header block first. (That would be a security
         * and correctness bug, since data in the middle of a volume may be
         * misinterpreted as a volume header!)
         */
        for (unsigned i = 1; i != numMapEntries; ++i) {
            FlashMapBlock b = map->blocks[i];
            ASSERT(b.isValid() == false || map->blocks[i].code > block.code);
        }
    })

    return true;
}

unsigned FlashVolume::getType() const
{
    ASSERT(isValid());
    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());
    return hdr->type;
}

FlashVolume FlashVolume::getParent() const
{
    ASSERT(isValid());
    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());
    return FlashMapBlock::fromCode(hdr->parentBlock);
}

FlashMapSpan FlashVolume::getPayload(FlashBlockRef &ref) const
{
    ASSERT(isValid());
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());

    unsigned numMapEntries = hdr->numMapEntries();
    unsigned size = hdr->payloadBlocks;
    unsigned offset = FlashVolumeHeader::payloadOffsetBlocks(numMapEntries, hdr->dataBytes);
    const FlashMap* map = hdr->getMap();

    return FlashMapSpan::create(map, offset, size);
}

uint8_t *FlashVolume::mapTypeSpecificData(FlashBlockRef &ref, unsigned &size) const
{
    /*
     * Return a pointer and size to type-specific data in our cache.
     * Restricted to only the portion of the type-specific data which
     * fits in the same cache block as the header.
     */

    ASSERT(isValid());
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());

    // Allow underflow in these calculations
    unsigned numMapEntries = hdr->numMapEntries();
    int32_t offset = hdr->dataOffsetBytes(numMapEntries);
    int32_t actualSize = FlashBlock::BLOCK_SIZE - offset;
    int32_t dataBytes = hdr->dataBytes;
    actualSize = MIN(actualSize, dataBytes);
    if (actualSize <= 0) {
        size = 0;
        return 0;
    }

    size = actualSize;
    return offset + (uint8_t*)hdr;
}

void FlashVolume::deleteSingle() const
{
    deleteSingleWithoutInvalidate();

    // Must notify LFS that we deleted a volume
    FlashLFSCache::invalidate();
}

void FlashVolume::deleteSingleWithoutInvalidate() const
{
    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, block);
    ASSERT(hdr->isHeaderValid());

    // If we're deleting a user-visible volume, send out a change event.
    if (typeIsUserCreated(hdr->type))
        Event::setBasePending(Event::PID_BASE_VOLUME_DELETE, getHandle());

    FlashBlockWriter writer(ref);
    hdr->type = T_DELETED;
    hdr->typeCopy = T_DELETED;
}

void FlashVolume::deleteTree() const
{
    /*
     * Delete a volume and all of its children. This is equivalent to a
     * recursive delete operation, but to conserve stack space we actually
     * do this iteratively using a bit vector to keep track of pending
     * deletes.
     *
     * This also has the benefit of letting us process one complete level
     * of the deletion tree for each iteration over the volume list,
     * instead of requiring an iteration for every single delete operation.
     */

    FlashMapBlock::Set deleted;
    bool notDoneYet;
    deleted.clear();

    // This is the root of the deletion tree
    deleteSingle();
    block.mark(deleted);

    do {
        FlashVolumeIter vi;
        FlashVolume vol;

        /*
         * Visit volumes in order, first to read their parent and then to
         * potentially delete them. By doing both of these consecutively on
         * the same volume, we can reduce cache thrashing.
         *
         * To finish, we need to do a full scan in which we find no volumes
         * with deleted parents. We need this one last scan, since there's
         * no way to know if a volume we've already iterated past was parented
         * to a volume that was marked for deletion later in the same scan.
         */

        notDoneYet = false;
        vi.begin();

        while (vi.next(vol)) {
            FlashVolume parent = vol.getParent();

            if (!deleted.test(vol.block.index()) &&
                parent.block.isValid() &&
                deleted.test(parent.block.index())) {

                vol.deleteSingle();
                vol.block.mark(deleted);
                notDoneYet = true;
            }
        }
    } while (notDoneYet);
}

void FlashVolume::deleteEverything()
{
    /*
     * Delete all volumes, but of course leave erase count data in place.
     * This operation is fairly speedy, since we don't actually erase
     * any memory right away.
     */

    FlashVolumeIter vi;
    FlashVolume vol;
    vi.begin();
    while (vi.next(vol))
        vol.deleteSingle();

    SysLFS::invalidateClients();
}

bool FlashVolumeIter::next(FlashVolume &vol)
{
    unsigned index;

    ASSERT(initialized == true);

    while (remaining.clearFirst(index)) {
        FlashVolume v(FlashMapBlock::fromIndex(index));

        if (v.isValid()) {
            FlashBlockRef ref;
            FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, v.block);
            ASSERT(hdr->isHeaderValid());
            const FlashMap *map = hdr->getMap();

            // Don't visit any future blocks that are part of this volume
            for (unsigned I = 0, E = hdr->numMapEntries(); I != E; ++I) {
                FlashMapBlock block = map->blocks[I];
                if (block.isValid())
                    block.clear(remaining);
            }

            vol = v;
            return true;
        }
    }

    return false;
}

bool FlashVolumeWriter::begin(FlashBlockRecycler &recycler,
    unsigned type, unsigned payloadBytes, unsigned hdrDataBytes, FlashVolume parent)
{
    // The real type will be written in commit(), once the volume is complete.
    this->type = type;
    this->payloadBytes = payloadBytes;
    this->payloadOffset = 0;

    // Start building a volume header, in anonymous cache memory.
    // populateMap() will assign concrete block addresses to the volume.

    FlashBlockWriter writer;
    writer.beginBlock();

    unsigned payloadBlocks = (payloadBytes + FlashBlock::BLOCK_MASK) / FlashBlock::BLOCK_SIZE;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(writer.ref);
    hdr->init(FlashVolume::T_INCOMPLETE, payloadBlocks, hdrDataBytes, parent.block);

    // Fill the volume header's map with recycled memory blocks
    unsigned numMapEntries = hdr->numMapEntries();
    unsigned count = populateMap(writer, recycler, numMapEntries, volume);
    if (count == 0) {
        // Didn't allocate anything successfully, not even a header
        return false;
    }

    // Finish writing
    writer.commitBlock();
    ASSERT(volume.isValid());

    return count == numMapEntries;
}

unsigned FlashVolumeWriter::populateMap(FlashBlockWriter &hdrWriter,
    FlashBlockRecycler &recycler, unsigned count, FlashVolume &hdrVolume)
{
    /*
     * Get some temporary memory to store erase counts in.
     *
     * We know that the map and header fit in one block, but the erase
     * counts have no guaranteed block-level alignment. To keep this simple,
     * we'll store them as an aligned and packed array in anonymous RAM,
     * which we'll later write into memory which may or may not overlap with
     * the header block.
     */

    FlashVolumeHeader *hdr = FlashVolumeHeader::get(hdrWriter.ref);
    ASSERT(count == hdr->numMapEntries());

    const unsigned ecPerBlock = FlashBlock::BLOCK_SIZE / sizeof(FlashVolumeHeader::EraseCount);
    const unsigned ecNumBlocks = FlashMap::NUM_MAP_BLOCKS / ecPerBlock;

    FlashBlockRef ecBlocks[ecNumBlocks];
    for (unsigned i = 0; i != ecNumBlocks; ++i)
        FlashBlock::anonymous(ecBlocks[i]);

    /*
     * Start filling both the map and the temporary erase count array.
     *
     * Note that we can fail to allocate a block at any point, but we
     * need to try our best to avoid losing any erase count data in the
     * event of an allocation failure or power loss. To preserve the
     * erase count data, we follow through with allocating a volume
     * of the T_INCOMPLETE type.
     */

    FlashMap *map = hdr->getMap();
    unsigned actualCount;

    for (actualCount = 0; actualCount < count; ++actualCount) {
        FlashMapBlock block;
        FlashBlockRecycler::EraseCount ec;

        if (!recycler.next(block, ec))
            break;

        DEBUG_ONLY(block.verifyErased();)

        /*
         * We must ensure that the first block has the lowest block index,
         * otherwise FlashVolumeIter might find a non-header block first.
         * (That would be a security and correctness bug, since data in the
         * middle of a volume may be misinterpreted as a volume header!)
         */

        uint32_t *ecHeader = reinterpret_cast<uint32_t*>(ecBlocks[0]->getData());
        uint32_t *ecThis = reinterpret_cast<uint32_t*>(ecBlocks[actualCount / ecPerBlock]->getData())
            + (actualCount % ecPerBlock);

        if (actualCount && block.index() < map->blocks[0].index()) {
            // New lowest block index, swap.

            map->blocks[actualCount] = map->blocks[0];
            map->blocks[0] = block;

            *ecThis = *ecHeader;
            *ecHeader = ec;

        } else {
            // Normal append

            map->blocks[actualCount] = block;
            *ecThis = ec;
        }
    }

    if (actualCount == 0) {
        // Not even successful at allocating a header!
        return 0;
    }

    // Save header address
    hdrVolume = map->blocks[0];

    /*
     * Now that we know the correct block order, we can finalize the header.
     *
     * We need to write a correct header with good CRCs, map, and erase counts
     * even if the overall populateMap() operation is not completely successful.
     * Also note that the erase counts may or may not overlap with the header
     * block. We use FlashBlockWriter to manage this complexity.
     */

    // Assign a real address to the header
    hdrWriter.relocate(hdrVolume.block.address());

    hdr->crcMap = hdr->calculateMapCRC(count);

    // Calculate erase count CRC from our anonymous memory array
    Crc32::reset();
    for (unsigned I = 0; I < count; ++I) {
        FlashBlockRecycler::EraseCount *ec;
        ec = reinterpret_cast<uint32_t*>(ecBlocks[I / ecPerBlock]->getData()) + (I % ecPerBlock);

        if (I >= actualCount) {
            // Missing block
            *ec = 0;
        }

        Crc32::add(*ec);
    }
    hdr->crcErase = Crc32::get();

    /*
     * Now start writing erase counts, which may be in different blocks.
     * Note that this implicitly releases the reference we hold to "hdr".
     * Must save dataBytes and numMapEntries before this!
     */

    unsigned dataBytes = hdr->dataBytes;

    for (unsigned I = 0; I < count; ++I) {
        FlashBlockRecycler::EraseCount *ec;
        ec = reinterpret_cast<uint32_t*>(ecBlocks[I / ecPerBlock]->getData()) + (I % ecPerBlock);
        unsigned addr = FlashVolumeHeader::eraseCountAddress(hdrVolume.block, I, count, dataBytes);
        *hdrWriter.getData<FlashBlockRecycler::EraseCount>(addr) = *ec;
    }

    return actualCount;
}

void FlashVolumeWriter::commit()
{
    // Finish writing the payload first, if necessary

    payloadWriter.commitBlock();

    // Just rewrite the header block, this time with the correct 'type'.

    FlashBlockRef ref;
    FlashVolumeHeader *hdr = FlashVolumeHeader::get(ref, volume.block);

    FlashBlockWriter writer(ref);
    hdr->setType(type);
    writer.commitBlock();

    ASSERT(volume.isValid());

    // All done. Notify userspace, if they can see this volume.
    if (volume.typeIsUserCreated(type))
        Event::setBasePending(Event::PID_BASE_VOLUME_COMMIT, volume.getHandle());
}

uint8_t *FlashVolumeWriter::mapTypeSpecificData(unsigned &size)
{
    FlashBlockRef ref;
    uint8_t *data = volume.mapTypeSpecificData(ref, size);
    payloadWriter.beginBlock(ref);
    return data;
}

void FlashVolumeWriter::appendPayload(const uint8_t *bytes, uint32_t count)
{
    if (payloadOffset > payloadBytes ||
        count > payloadBytes ||
        payloadOffset + count > payloadBytes) {

        /*
         * If we're overrunning the end of the volume, it's an error!
         * Prevent the write from happening, and make sure payloadOffset does get
         * incremented so that isPayloadComplete() will never return 'true'.
         */

        payloadOffset += count;
        return;
    }

    FlashBlockRef spanRef;
    FlashMapSpan span = volume.getPayload(spanRef);

    while (count) {
        uint32_t chunk = count;
        FlashBlockRef dataRef;
        FlashMapSpan::PhysAddr pa;
        unsigned flags = 0;

        if ((payloadOffset & FlashBlock::BLOCK_MASK) == 0) {
            // We know this is the beginning of a fully erased block
            flags |= FlashBlock::F_KNOWN_ERASED;
        }

        if (!span.getBytes(dataRef, payloadOffset, pa, chunk, flags)) {
            // This shouldn't happen unless we're writing past the end of the span!
            ASSERT(0);
            return;
        }

        payloadWriter.beginBlock(dataRef);
        memcpy(pa, bytes, chunk);

        count -= chunk;
        payloadOffset += chunk;
        bytes += chunk;
    }
}

FlashVolume::FlashVolume(_SYSVolumeHandle vh)
{
    /*
     * Convert from _SYSVolumeHandle to a FlashVolume.
     *
     * The volume must be tested with isValid() to see if the handle
     * was actually valid at all.
     */

    if (signHandle(vh) == vh)
        block.code = vh;
    else
        block.setInvalid();
}

uint32_t FlashVolume::signHandle(uint32_t h)
{
    /*
     * This is part of hte implementation of FlashVolume::getHandle()
     * and its inverse, FlashVolume::FlashVolume(_SYSVolumeHandle).
     *
     * These are opaque 32-bit identifiers. Currently our FlashBlockMap code
     * is only 8 bits wide, so we have an extra 24 bits to play with in
     * converting FlashVolumes to userspace handles.
     *
     * In order to more strongly enforce their opacity and prevent bogus
     * FlashVolumes from getting inadvertently passed in from userspace,
     * we include a machine-specific hash in these upper bits. This function
     * replaces the upper bits of 'h' with a freshly calculated hash of
     * the lower bits, and returns the new value.
     *
     * This is not at all a secure algorithm. A savvy userspace programmer
     * could always either just brute-force attack the CRC, or they could
     * reverse-engineer the key values from a collection of valid volume
     * handles. But this will certainly prevent casual mistakes from giving
     * us bogus FlashVolumes.
     *
     * This function uses the CRC hardware.
     */

    FlashMapBlock b;
    STATIC_ASSERT(sizeof(b.code) == 1);   
    b.code = h;

    Crc32::reset();

    // Volume code
    Crc32::add(b.code);

    // Static "secret key"
    Crc32::add(0xe30f4f8e);

    // Unique hardware-specific key
    Crc32::addUniqueness();

    return b.code | (Crc32::get() & 0xFFFFFF00);
}

bool FlashVolumeWriter::beginGame(unsigned payloadBytes, const char *package)
{
    /**
     * Start writing a game, after deleting any existing game volumes with
     * the same package name.
     */

    SysLFS::cleanupDeletedVolumes();

    FlashVolumeIter vi;
    FlashVolume vol;

    vi.begin();
    while (vi.next(vol)) {
        if (vol.getType() != FlashVolume::T_GAME)
            continue;

        FlashBlockRef ref;
        Elf::Program program;
        if (program.init(vol.getPayload(ref))) {
            const char *str = program.getMetaString(ref, _SYS_METADATA_PACKAGE_STR);
            if (str && !strcmp(str, package))
                vol.deleteTree();
        }
    }

    FlashBlockRecycler recycler;
    return begin(recycler, FlashVolume::T_GAME, payloadBytes);
}

bool FlashVolumeWriter::beginLauncher(unsigned payloadBytes)
{
    /**
     * Start writing the launcher, after deleting any existing launcher.
     */

    SysLFS::cleanupDeletedVolumes();

    FlashVolumeIter vi;
    FlashVolume vol;

    vi.begin();
    while (vi.next(vol)) {
        if (vol.getType() == FlashVolume::T_LAUNCHER)
            vol.deleteTree();
    }

    FlashBlockRecycler recycler;
    return begin(recycler, FlashVolume::T_LAUNCHER, payloadBytes);
}
