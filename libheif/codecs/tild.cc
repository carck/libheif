/*
 * HEIF codec.
 * Copyright (c) 2024 Dirk Farin <dirk.farin@gmail.com>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tild.h"
#include "context.h"
#include "file.h"
#include <algorithm>
#include <security_limits.h>


static uint64_t readvec(const std::vector<uint8_t>& data, size_t& ptr, int len)
{
  uint64_t val = 0;
  while (len--) {
    val <<= 8;
    val |= data[ptr++];
  }

  return val;
}


void TildHeader::set_parameters(const heif_tild_image_parameters& params)
{
  m_parameters = params;

  m_offsets.resize(number_of_tiles());

  for (auto& tile : m_offsets) {
    tile.offset = TILD_OFFSET_NOT_AVAILABLE;
  }
}



Error TildHeader::parse(const std::shared_ptr<HeifFile>& file, heif_item_id tild_id)
{
  const Error eofError(heif_error_Invalid_input,
                       heif_suberror_Unspecified,
                       "Tild header data incomplete");

  std::vector<uint8_t> data;

#if 0
  const uint64_t APPROX_TILD_HEADER_SIZE = 1024;
  uint64_t maxDataLen = file->request_iloc_data(tild_id, 0, APPROX_TILD_HEADER_SIZE);
#endif

  Error err;
  err = file->append_data_from_iloc(tild_id, data, 0, 3);
  if (err) {
    return err;
  }

  assert(data.size() == 3);

#if 0
  if (data.size() < 2 + 1 + 2 * 4 + 2 * 4 + 4) {
    return eofError;
  }
#endif

  size_t idx = 0;
  version = data[idx++];
  if (version != 1) {
    std::stringstream sstr;
    sstr << "'tild' image version " << ((int) version) << " is not implemented yet";

    return {heif_error_Unsupported_feature,
            heif_suberror_Unsupported_data_version,
            sstr.str()};
  }

  int flags = data[idx++];

  switch (flags & 0x03) {
    case 0:
      m_parameters.offset_field_length = 32;
      break;
    case 1:
      m_parameters.offset_field_length = 40;
      break;
    case 2:
      m_parameters.offset_field_length = 48;
      break;
    case 3:
      m_parameters.offset_field_length = 64;
      break;
  }

  switch (flags & 0x0c) {
    case 0x00:
      m_parameters.size_field_length = 0;
      break;
    case 0x04:
      m_parameters.size_field_length = 24;
      break;
    case 0x08:
      m_parameters.size_field_length = 32;
      break;
    case 0xc0:
      m_parameters.size_field_length = 64;
      break;
  }

  m_parameters.tiles_are_sequential = !!(flags % 0x10);
  bool dimensions_are_64bit = (flags & 0x20);

  m_parameters.number_of_extra_dimensions = data[idx++];

  size_t size_of_header_without_offsets = 3 + (2 + m_parameters.number_of_extra_dimensions) * (dimensions_are_64bit ? 8 : 4) + 3 * 4;

  err = file->append_data_from_iloc(tild_id, data, 3, size_of_header_without_offsets - 3);
  if (err) {
    return err;
  }

#if 0
  if (data.size() < idx + 2 * (dimensions_are_64bit ? 8 : 4)) {
    return eofError;
  }

  if (data.size() < idx + (2 + m_parameters.number_of_extra_dimensions) * (dimensions_are_64bit ? 8 : 4) + 3 * 4) {
    return eofError;
  }
#endif

  m_parameters.image_width = readvec(data, idx, dimensions_are_64bit ? 8 : 4);
  m_parameters.image_height = readvec(data, idx, dimensions_are_64bit ? 8 : 4);

  if (m_parameters.image_width == 0 || m_parameters.image_height == 0) {
    return {heif_error_Invalid_input,
            heif_suberror_Unspecified,
            "'tild' image with zero width or height."};
  }

  for (int i = 0; i < m_parameters.number_of_extra_dimensions; i++) {
    uint64_t size = readvec(data, idx, dimensions_are_64bit ? 8 : 4);

    if (size == 0) {
      return {heif_error_Invalid_input,
              heif_suberror_Unspecified,
              "'tild' extra dimension may not be zero."};
    }

    if (i < 8) {
      m_parameters.extra_dimensions[i] = size;
    }
    else {
      // TODO: error: too many dimensions (not supported)
    }
  }

  m_parameters.tile_width = static_cast<uint32_t>(readvec(data, idx, 4));
  m_parameters.tile_height = static_cast<uint32_t>(readvec(data, idx, 4));

  m_parameters.compression_type_fourcc = static_cast<uint32_t>(readvec(data, idx, 4));

  if (m_parameters.tile_width == 0 || m_parameters.tile_height == 0) {
    return {heif_error_Invalid_input,
            heif_suberror_Unspecified,
            "Tile with zero width or height."};
  }

  uint64_t nTiles = number_of_tiles();
  if (nTiles > MAX_TILD_TILES) {
    return {heif_error_Invalid_input,
            heif_suberror_Security_limit_exceeded,
            "Number of tiles exceeds security limit."};
  }

  m_offsets.resize(nTiles);


  // --- load offsets (TODO: do this on-demand)

  size_t size_of_offset_table = nTiles * (m_parameters.offset_field_length + m_parameters.size_field_length) / 8;

  err = file->append_data_from_iloc(tild_id, data, size_of_header_without_offsets, size_of_offset_table);
  if (err) {
    return err;
  }

  for (uint64_t i=0; i<nTiles; i++) {
    m_offsets[i].offset = readvec(data, idx, m_parameters.offset_field_length/8);

    if (m_parameters.size_field_length) {
      assert(m_parameters.size_field_length <= 32);
      m_offsets[i].size = static_cast<uint32_t>(readvec(data, idx, m_parameters.size_field_length / 8));
    }

    // printf("[%zu] : offset/size: %zu %d\n", i, m_offsets[i].offset, m_offsets[i].size);
  }

  return Error::Ok;
}


uint64_t TildHeader::number_of_tiles() const
{
  uint64_t nTiles_h = (m_parameters.image_width + m_parameters.tile_width - 1) / m_parameters.tile_width;
  uint64_t nTiles_v = (m_parameters.image_height + m_parameters.tile_height - 1) / m_parameters.tile_height;
  uint64_t nTiles = nTiles_h * nTiles_v;

  for (int i = 0; i < m_parameters.number_of_extra_dimensions; i++) {
    // We only support up to 8 extra dimensions
    if (i == 8) {
      break;
    }

    nTiles *= m_parameters.extra_dimensions[i];
  }

  return nTiles;
}


uint64_t TildHeader::nTiles_h() const
{
  return (m_parameters.image_width + m_parameters.tile_width - 1) / m_parameters.tile_width;
}


uint64_t TildHeader::nTiles_v() const
{
  return (m_parameters.image_height + m_parameters.tile_height - 1) / m_parameters.tile_height;
}


size_t TildHeader::get_header_size() const
{
  assert(m_header_size);
  return m_header_size;
}


void TildHeader::set_tild_tile_range(uint32_t tile_x, uint32_t tile_y, uint64_t offset, uint32_t size)
{
  uint64_t idx = tile_y * nTiles_h() + tile_x;
  m_offsets[idx].offset = offset;
  m_offsets[idx].size = size;
}


template<typename I>
void writevec(uint8_t* data, size_t& idx, I value, int len)
{
  for (int i = 0; i < len; i++) {
    data[idx + i] = static_cast<uint8_t>((value >> (len - 1 - i) * 8) & 0xFF);
  }

  idx += len;
}


std::vector<uint8_t> TildHeader::write()
{
  assert(m_parameters.version == 1);

  uint8_t flags = 0;
  bool dimensions_are_64bit = false;

  if (m_parameters.image_width > 0xFFFF || m_parameters.image_height > 0xFFFF) {
    flags |= 0x20;
    dimensions_are_64bit = true;
  }

  switch (m_parameters.offset_field_length) {
    case 32:
      flags |= 0;
      break;
    case 40:
      flags |= 0x01;
      break;
    case 48:
      flags |= 0x02;
      break;
    case 64:
      flags |= 0x03;
      break;
    default:
      assert(false); // TODO: return error
  }

  switch (m_parameters.size_field_length) {
    case 0:
      flags |= 0;
      break;
    case 24:
      flags |= 0x04;
      break;
    case 32:
      flags |= 0x08;
      break;
    case 64:
      flags |= 0x0c;
      break;
    default:
      assert(false); // TODO: return error
  }

  printf("> %d %d -> %d\n", m_parameters.offset_field_length, m_parameters.size_field_length, (int)flags);

  if (m_parameters.tiles_are_sequential) {
    flags |= 0x10;
  }

  uint64_t nTiles = number_of_tiles();

  int offset_entry_size = (m_parameters.offset_field_length + m_parameters.size_field_length) / 8;

  std::vector<uint8_t> data;
  uint64_t size = (2 +  // version, flags
                   1 +  // number of extra dimensions
                   (dimensions_are_64bit ? 8 : 4) * (2 + m_parameters.number_of_extra_dimensions) + // image size
                   2 * 4 + // tile size
                   4 + // compression type
                   nTiles * offset_entry_size); // offsets

  data.resize(size);
  size_t idx = 0;
  data[idx++] = version;
  data[idx++] = flags;

  if (m_parameters.number_of_extra_dimensions > 8) {
    assert(false); // currently not supported
  }

  data[idx++] = m_parameters.number_of_extra_dimensions;

  writevec(data.data(), idx, m_parameters.image_width, dimensions_are_64bit ? 8 : 4);
  writevec(data.data(), idx, m_parameters.image_height, dimensions_are_64bit ? 8 : 4);

  for (int i = 0; i < m_parameters.number_of_extra_dimensions; i++) {
    writevec(data.data(), idx, m_parameters.extra_dimensions[i], dimensions_are_64bit ? 8 : 4);
  }

  writevec(data.data(), idx, m_parameters.tile_width, 4);
  writevec(data.data(), idx, m_parameters.tile_height, 4);

  writevec(data.data(), idx, m_parameters.compression_type_fourcc, 4);

  for (const auto& offset : m_offsets) {
    writevec(data.data(), idx, offset.offset, m_parameters.offset_field_length / 8);

    if (m_parameters.size_field_length != 0) {
      writevec(data.data(), idx, offset.size, m_parameters.size_field_length / 8);
    }
  }

  assert(idx == data.size());

  m_header_size = data.size();

  return data;
}


std::string TildHeader::dump() const
{
  std::stringstream sstr;

  sstr << "version: " << ((int) m_parameters.version) << "\n"
       << "image size: " << m_parameters.image_width << "x" << m_parameters.image_height << "\n"
       << "tile size: " << m_parameters.tile_width << "x" << m_parameters.tile_height << "\n"
       << "offsets: ";

  // TODO

  for (const auto& offset : m_offsets) {
    sstr << offset.offset << ", size: " << offset.size << "\n";
  }

  return sstr.str();
}


ImageItem_Tild::ImageItem_Tild(HeifContext* ctx)
    : ImageItem(ctx)
{
}


ImageItem_Tild::ImageItem_Tild(HeifContext* ctx, heif_item_id id)
    : ImageItem(ctx, id)
{
}


heif_compression_format ImageItem_Tild::get_compression_format() const
{
  return compression_format_from_fourcc_infe_type(m_tild_header.get_parameters().compression_type_fourcc);
}


Error ImageItem_Tild::on_load_file()
{
  Error err;
  auto heif_file = get_context()->get_heif_file();

  err = m_tild_header.parse(heif_file, get_id());
  if (err) {
    return err;
  }

  return Error::Ok;
}


Result<std::shared_ptr<ImageItem_Tild>> ImageItem_Tild::add_new_tild_item(HeifContext* ctx, const heif_tild_image_parameters* parameters)
{
  // Create header

  TildHeader tild_header;
  tild_header.set_parameters(*parameters);

  std::vector<uint8_t> header_data = tild_header.write();

  // Create 'tild' Item

  auto file = ctx->get_heif_file();

  heif_item_id tild_id = ctx->get_heif_file()->add_new_image("tild");
  auto tild_image = std::make_shared<ImageItem_Tild>(ctx, tild_id);
  ctx->insert_new_image(tild_id, tild_image);

  const int construction_method = 0; // 0=mdat 1=idat
  file->append_iloc_data(tild_id, header_data, construction_method);


  if (parameters->image_width > 0xFFFFFFFF || parameters->image_height > 0xFFFFFFFF) {
    return {Error(heif_error_Usage_error, heif_suberror_Invalid_image_size,
                  "'ispe' only supports image sized up to 4294967295 pixels per dimension")};
  }

  // Add ISPE property
  file->add_ispe_property(tild_id,
                          static_cast<uint32_t>(parameters->image_width),
                          static_cast<uint32_t>(parameters->image_height));

#if 0
  // TODO

  // Add PIXI property (copy from first tile)
  auto pixi = m_heif_file->get_property<Box_pixi>(tile_ids[0]);
  m_heif_file->add_property(grid_id, pixi, true);
#endif

  tild_image->set_tild_header(tild_header);
  tild_image->set_next_tild_position(header_data.size());

  // Set Brands
  //m_heif_file->set_brand(encoder->plugin->compression_format,
  //                       out_grid_image->is_miaf_compatible());

  return {tild_image};
}


void ImageItem_Tild::process_before_write()
{
  // overwrite offsets

  const int construction_method = 0; // 0=mdat 1=idat

  std::vector<uint8_t> header_data = m_tild_header.write();
  get_file()->replace_iloc_data(get_id(), 0, header_data, construction_method);
}


Result<std::shared_ptr<HeifPixelImage>> ImageItem_Tild::decode_compressed_image(const struct heif_decoding_options& options,
                                                                                bool decode_tile_only, uint32_t tile_x0, uint32_t tile_y0) const
{
  if (decode_tile_only) {
    return decode_grid_tile(options, tile_x0, tile_y0);
  }
  else {
    return Error{heif_error_Unsupported_feature, heif_suberror_Unspecified, "'tild' images can only be access per tile"};
  }
}


Result<std::shared_ptr<HeifPixelImage>> ImageItem_Tild::decode_grid_tile(const heif_decoding_options& options, uint32_t tx, uint32_t ty) const
{
  heif_compression_format format = compression_format_from_fourcc_infe_type(m_tild_header.get_parameters().compression_type_fourcc);

  // --- get compressed data

  Result<std::vector<uint8_t>> dataResult = read_bitstream_configuration_data_override(get_id(), format);
  if (dataResult.error) {
    return dataResult.error;
  }

  std::vector<uint8_t>& data = dataResult.value;

  // --- decode

  uint32_t idx = (uint32_t)(ty * m_tild_header.nTiles_h() + tx);

  uint64_t offset = m_tild_header.get_tile_offset(idx);
  uint64_t size = m_tild_header.get_tile_size(idx);

  Error err = get_file()->append_data_from_iloc(get_id(), data, offset, size);
  assert(!err.error_code);

  return decode_from_compressed_data(get_compression_format(), options, data);
}


heif_image_tiling ImageItem_Tild::get_heif_image_tiling() const
{
  heif_image_tiling tiling{};

  tiling.num_columns = m_tild_header.nTiles_h();
  tiling.num_rows = m_tild_header.nTiles_v();

  tiling.tile_width = m_tild_header.get_parameters().tile_width;
  tiling.tile_height = m_tild_header.get_parameters().tile_height;

  tiling.image_width = m_tild_header.get_parameters().image_width;
  tiling.image_height = m_tild_header.get_parameters().image_height;
  tiling.number_of_extra_dimensions = m_tild_header.get_parameters().number_of_extra_dimensions;
  for (int i = 0; i < std::min(tiling.number_of_extra_dimensions, uint8_t(8)); i++) {
    tiling.extra_dimensions[i] = m_tild_header.get_parameters().extra_dimensions[i];
  }

  return tiling;
}


void ImageItem_Tild::get_tile_size(uint32_t& w, uint32_t& h) const
{
  w = m_tild_header.get_parameters().tile_width;
  h = m_tild_header.get_parameters().tile_height;
}
