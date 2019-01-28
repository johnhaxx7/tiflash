#include <Storages/Transaction/RegionFile.h>

namespace DB
{

RegionFile::Writer::Writer(RegionFile & region_file)
    : data_file_size(region_file.file_size),
      data_file_buf(region_file.dataPath(), DBMS_DEFAULT_BUFFER_SIZE, O_APPEND | O_WRONLY | O_CREAT),
      index_file_buf(region_file.indexPath(), DBMS_DEFAULT_BUFFER_SIZE, O_APPEND | O_WRONLY | O_CREAT)
{
}

RegionFile::Writer::~Writer()
{
    // flush page cache.
    data_file_buf.sync();
    index_file_buf.sync();
}

size_t RegionFile::Writer::write(const RegionPtr & region)
{
    // index file format: [ region_id(8 bytes), region_size(8 bytes), reserve(8 bytes) ] , [ ... ]
    size_t region_size = region->serialize(data_file_buf);

    writeIntBinary(region->id(), index_file_buf);
    writeIntBinary(region_size, index_file_buf);
    writeIntBinary((UInt64)0, index_file_buf); // reserve 8 bytes

    data_file_size += region_size;

    return region_size;
}

RegionFile::Reader::Reader(RegionFile & region_file)
    : data_file_buf(region_file.dataPath(), std::min(region_file.file_size, static_cast<off_t>(DBMS_DEFAULT_BUFFER_SIZE)), O_RDONLY)
{
    // TODO: remove number 24576
    ReadBufferFromFile index_file_buf(region_file.indexPath(), 24576, O_RDONLY);

    while (!index_file_buf.eof())
    {
        auto region_id   = readBinary2<UInt64>(index_file_buf);
        auto region_size = readBinary2<UInt64>(index_file_buf);

        // reserve 8 bytes
        readBinary2<UInt64>(index_file_buf);

        metas.emplace_back(region_id, region_size);
    }
}

RegionID RegionFile::Reader::hasNext()
{
    if (cur_region_index >= metas.size())
        return InvalidRegionID;
    // TODO: hasNext should be a const method
    auto & meta = metas[cur_region_index++];
    cur_region_size = meta.region_size;
    return meta.region_id;
}

RegionPtr RegionFile::Reader::next()
{
    cur_region_offset += cur_region_size;
    return Region::deserialize(data_file_buf);
}

void RegionFile::Reader::skipNext()
{
    data_file_buf.seek(cur_region_offset + cur_region_size);
    cur_region_offset += cur_region_size;
}

bool RegionFile::tryCoverRegion(RegionID region_id, RegionFile & other)
{
    if (other.file_id == file_id) // myself
        return true;
    if (other.regions.find(region_id) == other.regions.end())
        return true;
    // Now both this and other contains the region, the bigger file_id wins.
    if (other.file_id > file_id)
        regions.erase(region_id); // cover myself
    else
        other.regions.erase(region_id); // cover other
    return false;
}

bool RegionFile::addRegion(RegionID region_id, size_t region_size)
{
    return !regions.insert_or_assign(region_id, region_size).second;
}

bool RegionFile::dropRegion(RegionID region_id)
{
    return regions.erase(region_id) != 0;
}

/// Remove underlying file and clean up resources.
void RegionFile::destroy()
{
    {
        Poco::File file(indexPath());
        if (file.exists())
            file.remove();
    }

    {
        Poco::File file(dataPath());
        if (file.exists())
            file.remove();
    }

    regions.clear();
    file_size = 0;
}

void RegionFile::resetId(UInt64 new_file_id)
{
    // TODO This could be partial succeed, need to fix.
    {
        Poco::File file(indexPath());
        if (file.exists())
            file.renameTo(indexPath(new_file_id));
    }

    {
        Poco::File file(dataPath());
        if (file.exists())
            file.renameTo(dataPath(new_file_id));
    }

    file_id = new_file_id;
}

Float64 RegionFile::useRate()
{
    size_t size = 0;
    for (auto & p : regions)
        size += p.second;
    return ((Float64)size) / file_size;
}

std::string RegionFile::dataPath()
{
    return dataPath(file_id);
}

std::string RegionFile::indexPath()
{
    return indexPath(file_id);
}

std::string RegionFile::dataPath(UInt64 the_file_id)
{
    return parent_path + DB::toString(the_file_id) + REGION_DATA_FILE_SUFFIX;
}

std::string RegionFile::indexPath(UInt64 the_file_id)
{
    return parent_path + DB::toString(the_file_id) + REGION_INDEX_FILE_SUFFIX;
}

} // namespace DB