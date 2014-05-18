#include "parquet/parquet.h"
#include "encodings.h"

#include <string>
#include <string.h>

#include <thrift/protocol/TDebugProtocol.h>

const int DATA_PAGE_SIZE = 64 * 1024;

using namespace boost;
using namespace parquet;
using namespace std;

namespace parquet_cpp {

InMemoryInputStream::InMemoryInputStream(const uint8_t* buffer, int64_t len) :
  buffer_(buffer), len_(len), offset_(0) {
}

const uint8_t* InMemoryInputStream::Peek(int num_to_peek, int* num_bytes) {
  *num_bytes = ::min(static_cast<int64_t>(num_to_peek), len_ - offset_);
  return buffer_ + offset_;
}

const uint8_t* InMemoryInputStream::Read(int num_to_read, int* num_bytes) {
  const uint8_t* result = Peek(num_to_read, num_bytes);
  offset_ += *num_bytes;
  return result;
}

ColumnReader::ColumnReader(const SchemaElement* schema, InputStream* stream)
  : schema_(schema),
    stream_(stream),
    num_buffered_values_(0),
    current_decoder_(NULL) {
}

int32_t ColumnReader::GetInt32(int* definition_level, int* repetition_level) {
  --num_buffered_values_;
  *repetition_level = 1;
  if (!definition_level_decoder_.Get(definition_level)) ParquetException::EofException();
  if (*definition_level == 0) return true;
  return current_decoder_->GetInt32();
}

static bool IsDictionaryIndexEncoding(const Encoding::type& e) {
  // PLAIN_DICTIONARY is deprecated but used to be used as a dictionary index
  // encoding.
  return e == Encoding::RLE_DICTIONARY || e == Encoding::PLAIN_DICTIONARY;
}

bool ColumnReader::ReadNewPage() {
  // Loop until we find the next data page.
  while (true) {
    int bytes_read = 0;
    const uint8_t* buffer = stream_->Peek(DATA_PAGE_SIZE, &bytes_read);
    if (bytes_read == 0) return false;
    uint32_t header_size = bytes_read;
    if (!DeserializeThriftMsg(buffer, &header_size, &current_page_header_)) {
      return false;
    }
    stream_->Read(header_size, &bytes_read);

    // TODO: handle decompression.
    int uncompressed_len = current_page_header_.uncompressed_page_size;
    buffer = stream_->Read(uncompressed_len, &bytes_read);
    if (bytes_read != uncompressed_len) ParquetException::EofException();

    if (current_page_header_.type == PageType::DICTIONARY_PAGE) {
      unordered_map<Encoding::type, shared_ptr<Decoder> >::iterator it =
          decoders_.find(Encoding::RLE_DICTIONARY);
      if (it != decoders_.end()) {
        throw ParquetException("Column cannot have more than one dictionary.");
      }

      PlainDecoder dictionary(schema_);
      dictionary.SetData(current_page_header_.dictionary_page_header.num_values,
          buffer, uncompressed_len);
      shared_ptr<Decoder> decoder(new DictionaryDecoder(schema_, &dictionary));
      decoders_[Encoding::RLE_DICTIONARY] = decoder;
      current_decoder_ = decoders_[Encoding::RLE_DICTIONARY].get();
      continue;
    } else if (current_page_header_.type == PageType::DATA_PAGE) {
      // Read a data page.
      num_buffered_values_ = current_page_header_.data_page_header.num_values;

      // Read definition levels.
      int num_definition_bytes = *reinterpret_cast<const uint32_t*>(buffer);
      buffer += sizeof(uint32_t);
      definition_level_decoder_ = impala::RleDecoder(buffer, num_definition_bytes, 1);
      buffer += num_definition_bytes;

      // TODO: repetition levels

      // Get a decoder object for this page or create a new decoder if this is the
      // first page with this encoding.
      Encoding::type encoding = current_page_header_.data_page_header.encoding;
      if (IsDictionaryIndexEncoding(encoding)) encoding = Encoding::RLE_DICTIONARY;

      unordered_map<Encoding::type, shared_ptr<Decoder> >::iterator it =
          decoders_.find(encoding);
      if (it != decoders_.end()) {
        current_decoder_ = it->second.get();
      } else {
        switch (encoding) {
          case Encoding::PLAIN: {
            shared_ptr<Decoder> decoder(new PlainDecoder(schema_));
            decoders_[encoding] = decoder;
            current_decoder_ = decoder.get();
            break;
          }
          case Encoding::RLE_DICTIONARY:
            throw ParquetException("Dictionary page must be before data page.");

          case Encoding::DELTA_BINARY_PACKED:
          case Encoding::DELTA_LENGTH_BYTE_ARRAY:
          case Encoding::DELTA_BYTE_ARRAY:
            ParquetException::NYI();

          default:
            throw ParquetException("Unknown encoding type.");
        }
      }
      current_decoder_->SetData(num_buffered_values_, buffer,
          uncompressed_len - sizeof(uint32_t) - num_definition_bytes);
    } else {
      // We don't know what this page type is. just skip it.
      continue;
    }
  }
  return true;
}

bool ColumnReader::HasNext() {
  if (num_buffered_values_ == 0) {
    ReadNewPage();
    if (num_buffered_values_ == 0) return false;
  }
  return true;
}

}
