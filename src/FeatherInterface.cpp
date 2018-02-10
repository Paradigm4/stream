/*
**
* BEGIN_COPYRIGHT
*
* Copyright (C) 2008-2017 SciDB, Inc.
* All Rights Reserved.
*
* stream is a plugin for SciDB, an Open Source Array DBMS maintained
* by Paradigm4. See http://www.paradigm4.com/
*
* stream is free software: you can redistribute it and/or modify
* it under the terms of the AFFERO GNU General Public License as published by
* the Free Software Foundation.
*
* stream is distributed "AS-IS" AND WITHOUT ANY WARRANTY OF ANY KIND,
* INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,
* NON-INFRINGEMENT, OR FITNESS FOR A PARTICULAR PURPOSE. See
* the AFFERO GNU General Public License for the complete license terms.
*
* You should have received a copy of the AFFERO GNU General Public License
* along with stream.  If not, see <http://www.gnu.org/licenses/agpl-3.0.html>
*
* END_COPYRIGHT
*/

#include <vector>
#include <string>
#include <memory>
#include <arrow/api.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/feather.h>

#include "StreamSettings.h"
#include "ChildProcess.h"
#include "FeatherInterface.h"

using std::vector;
using std::shared_ptr;
using std::string;

namespace scidb { namespace stream {

static log4cxx::LoggerPtr logger(
    log4cxx::Logger::getLogger("scidb.operators.stream.feather_interface"));

ArrayDesc FeatherInterface::getOutputSchema(
    std::vector<ArrayDesc> const& inputSchemas,
    Settings const& settings,
    std::shared_ptr<Query> const& query)
{
    if(settings.getFormat() != FEATHER)
    {
        throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION)
            << "FEATHER interface invoked on improper format";
    }
    vector<TypeEnum> outputTypes = settings.getTypes();
    vector<string>   outputNames = settings.getNames();
    if(outputTypes.size() == 0)
    {
        throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION)
            << "FEATHER interface requires that output types are specified";
    }
    if(outputNames.size() == 0)
    {
        for(size_t i =0; i<outputTypes.size(); ++i)
        {
            ostringstream name;
            name << "a" << i;
            outputNames.push_back(name.str());
        }
    }
    else if (outputNames.size() != outputTypes.size())
    {
        throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION)
            << "received inconsistent names and types";
    }
    for(size_t i = 0; i<inputSchemas.size(); ++i)
    {
        ArrayDesc const& schema = inputSchemas[i];
        Attributes const& attrs = schema.getAttributes(true);
        for(size_t j = 0, nAttrs = attrs.size(); j<nAttrs; ++j)
        {
            AttributeDesc const& attr = attrs[j];
            TypeEnum te = typeId2TypeEnum(attrs[j].getType(), true);
        }
    }
    Dimensions outputDimensions;
    outputDimensions.push_back(
        DimensionDesc(
            "instance_id", 0, query->getInstancesCount() - 1, 1, 0));
    outputDimensions.push_back(
        DimensionDesc(
            "chunk_no", 0, CoordinateBounds::getMax(), 1, 0));
    outputDimensions.push_back(DimensionDesc("value_no",
                                             0,
                                             CoordinateBounds::getMax(),
                                             settings.getChunkSize(),
                                             0));
    Attributes outputAttributes;
    for(AttributeID i =0; i<outputTypes.size(); ++i)
    {
        outputAttributes.push_back(
            AttributeDesc(i,
                          outputNames[i],
                          typeEnum2TypeId(outputTypes[i]),
                          AttributeDesc::IS_NULLABLE, 0));
    }
    outputAttributes = addEmptyTagAttribute(outputAttributes);
    return ArrayDesc(inputSchemas[0].getName(),
                     outputAttributes,
                     outputDimensions,
                     defaultPartitioning(),
                     query->getDefaultArrayResidency());
}

FeatherInterface::FeatherInterface(Settings const& settings,
                                   ArrayDesc const& outputSchema,
                                   std::shared_ptr<Query> const& query):
    _query(query),
    _result(new MemArray(outputSchema, query)),
    _outPos{((Coordinate) _query->getInstanceID()), 0, 0},
    _outputChunkSize(settings.getChunkSize()),
    _nOutputAttrs((int32_t)outputSchema.getAttributes(true).size()),
    _oaiters(_nOutputAttrs + 1),
    _outputTypes(_nOutputAttrs),
    _readBuf(1024*1024)
{
    for(int32_t i = 0; i < _nOutputAttrs; ++i)
    {
        _oaiters[i] = _result->getIterator(i);
        _outputTypes[i] = settings.getTypes()[i];
    }
    _oaiters[_nOutputAttrs] = _result->getIterator(_nOutputAttrs);
    _nullVal.setNull();
}

void FeatherInterface::setInputSchema(ArrayDesc const& inputSchema)
{
    Attributes const& attrs = inputSchema.getAttributes(true);
    size_t const nInputAttrs = attrs.size();
    _inputTypes.resize(nInputAttrs);
    _inputNames.resize(nInputAttrs);
    _inputConverters.resize(nInputAttrs);
    for(size_t i=0; i < nInputAttrs; ++i)
    {
        TypeId const& inputType = attrs[i].getType();
        _inputTypes[i] = typeId2TypeEnum(inputType, true);
        switch(_inputTypes[i])
        {
        case TE_BOOL:
        case TE_DOUBLE:
        case TE_FLOAT:
        case TE_UINT8:
        case TE_INT8:
        case TE_BINARY:
            _inputConverters[i] = NULL;
            break;
        default:
            _inputConverters[i] = FunctionLibrary::getInstance()->findConverter(
                inputType,
                TID_STRING,
                false);
        }
        _inputNames[i]= attrs[i].getName();
    }
}

void FeatherInterface::streamData(
    std::vector<ConstChunk const*> const& inputChunks,
    ChildProcess& child)
{
    if(inputChunks.size() != _inputTypes.size())
    {
        throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION)
          << "received inconsistent number of input chunks";
    }
    size_t numRows = inputChunks[0]->count();
    if(numRows == 0)
    {
        return;
    }
    if(numRows > (size_t) std::numeric_limits<int32_t>::max())
    {
        throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION)
          << "received chunk with count exceeding the Arrow array limit";
    }
    if(!child.isAlive())
    {
        throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION)
          << "child exited early";
    }
    writeFeather(inputChunks, numRows, child);
    readFeather(child);
}

shared_ptr<Array> FeatherInterface::finalize(ChildProcess& child)
{
    writeFinalFeather(child);
    readFeather(child, true);
    _oaiters.clear();
    return _result;
}

void FeatherInterface::writeFeather(vector<ConstChunk const*> const& chunks,
                                    int32_t const numRows,
                                    ChildProcess& child)
{
    int32_t numColumns = chunks.size();
    LOG4CXX_DEBUG(logger, "writeFeather::numColumns:" << numColumns);
    LOG4CXX_DEBUG(logger, "writeFeather::numRows:" << numRows);

    std::shared_ptr<arrow::io::BufferOutputStream> stream;
    arrow::io::BufferOutputStream::Create(
        1024, arrow::default_memory_pool(), &stream);

    std::unique_ptr<arrow::ipc::feather::TableWriter> writer;
    arrow::ipc::feather::TableWriter::Open(stream, &writer);

    writer->SetNumRows(numRows);

    for(size_t i = 0; i < _inputTypes.size(); ++i)
    {
        shared_ptr<ConstChunkIterator> citer =
          chunks[i]->getConstIterator(ConstChunkIterator::IGNORE_OVERLAPS
                                      | ConstChunkIterator::IGNORE_EMPTY_CELLS);

        std::shared_ptr<arrow::Array> array;
        switch(_inputTypes[i])
        {
        case TE_INT64:
        {
            arrow::Int64Builder builder;

            while((!citer->end()))
            {
                Value const& value = citer->getItem();
                if(value.isNull())
                {
                    builder.AppendNull();
                }
                else
                {
                    builder.Append(value.getInt64());
                }
                ++(*citer);
            }

            builder.Finish(&array);
            break;
        }
        case TE_DOUBLE:
        {
            arrow::DoubleBuilder builder;

            while((!citer->end()))
            {
                Value const& value = citer->getItem();
                if(value.isNull())
                {
                    builder.AppendNull();
                }
                else
                {
                    builder.Append(value.getDouble());
                }
                ++(*citer);
            }

            builder.Finish(&array);
            break;
        }
        case TE_STRING:
        {
            arrow::StringBuilder builder;

            while((!citer->end()))
            {
                Value const& value = citer->getItem();
                if(value.isNull())
                {
                    builder.AppendNull();
                }
                else
                {
                    builder.Append(value.getString());
                }
                ++(*citer);
            }

            builder.Finish(&array);
            break;
        }
        case TE_BINARY:
        {
            arrow::BinaryBuilder builder;

            while((!citer->end()))
            {
                Value const& value = citer->getItem();
                if(value.isNull())
                {
                    builder.AppendNull();
                }
                else
                {
                    builder.Append((const uint8_t*)value.data(), value.size());
                }
                ++(*citer);
            }

            builder.Finish(&array);
            break;
        }
        default: throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL,
                                        SCIDB_LE_ILLEGAL_OPERATION)
            << "internal error: unsupported type";
        }

        // Print array for debugging
        std::stringstream prettyprint_stream;
        arrow::PrettyPrint(*array, 0, &prettyprint_stream);
        LOG4CXX_DEBUG(logger, "writeFeather::array:"
                      << prettyprint_stream.str().c_str());

        writer->Append(_inputNames[i].c_str(), *array);
    }
    writer->Finalize();

    std::shared_ptr<arrow::Buffer> buffer;
    stream->Finish(&buffer);

    uint64_t writeSize = buffer->size();
    LOG4CXX_DEBUG(logger, "writeFeather::writeSize:" << writeSize);
    child.hardWrite(&writeSize, sizeof(uint64_t));
    child.hardWrite(buffer->data(), writeSize);
}

void FeatherInterface::writeFinalFeather(ChildProcess& child)
{
    LOG4CXX_DEBUG(logger, "writeFinalFeather::0");

    int64_t zero = 0;
    child.hardWrite(&zero, sizeof(int64_t));
}

void FeatherInterface::readFeather(ChildProcess& child,
                                   bool lastMessage)
{
    LOG4CXX_DEBUG(logger, "readFeather");

    uint64_t readSize;
    child.hardRead(&readSize, sizeof(uint64_t), !lastMessage);
    LOG4CXX_DEBUG(logger, "readFeather::readSize:" << readSize);
    if (readSize == 0)
    {
        return;
    }

    if (readSize > _readBuf.size())
    {
        _readBuf.resize(readSize);
    }
    child.hardRead(&(_readBuf[0]), readSize, !lastMessage);
    std::shared_ptr<arrow::io::BufferReader> buffer(
        new arrow::io::BufferReader(&(_readBuf[0]), readSize));

    std::unique_ptr<arrow::ipc::feather::TableReader> reader;
    arrow::ipc::feather::TableReader::Open(buffer, &reader);

    int64_t numColumns = reader->num_columns();
    int64_t numRows = reader->num_rows();
    LOG4CXX_DEBUG(logger, "readFeather::numColumns:" << numColumns
                  << ":numRows:" << numRows);

    if (numColumns > 0 && numColumns != _nOutputAttrs)
    {
        throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION)
            << "received incorrect number of columns";
    }
    if (numColumns == 0 || numRows == 0)
    {
        return;
    }

    std::shared_ptr<arrow::Column> col;
    for(int64_t i = 0; i < numColumns; ++i)
    {
        LOG4CXX_DEBUG(logger, "readFeather::column:" << i);

        reader->GetColumn(i, &col);
        // Feather files have only one chunk
        // http://mail-archives.apache.org/mod_mbox/arrow-dev/201709.mbox/%3CCAJPUwMApjFdQFaiTXHYZJJCGcndrPn95UESS1ptDeWZ1zURubQ%40mail.gmail.com%3E
        std::shared_ptr<arrow::Array> array(col->data()->chunk(0));
        int64_t nullCount = array->null_count();
        const uint8_t* nullBitmap = array->null_bitmap_data();

        LOG4CXX_DEBUG(logger, "readFeather::array:" << *array);
        LOG4CXX_DEBUG(logger, "readFeather::array.null:" << nullCount);

        shared_ptr<ChunkIterator> ociter = _oaiters[i]->newChunk(
            _outPos).getIterator(_query,
                                 ChunkIterator::SEQUENTIAL_WRITE
                                 | ChunkIterator::NO_EMPTY_CHECK);
        Coordinates valPos = _outPos;

        switch(_outputTypes[i])
        {
        case TE_INT64:
        {
            const int64_t* arrayData =
                std::static_pointer_cast<arrow::Int64Array>(
                    array)->raw_values();

            for(int64_t j = 0; j < numRows; ++j)
            {
                ociter->setPosition(valPos);
                if (nullCount != 0 && ! (nullBitmap[j / 8] & 1 << j % 8))
                {
                    ociter->writeItem(_nullVal);
                }
                else
                {
                    _val.setInt64(arrayData[j]);
                    ociter->writeItem(_val);
                }
                ++valPos[2];
            }
            break;
        }
        case TE_DOUBLE:
        {
            const double* arrayData =
                std::static_pointer_cast<arrow::DoubleArray>(
                    array)->raw_values();

            for(int64_t j = 0; j < numRows; ++j)
            {
                ociter->setPosition(valPos);
                if (nullCount != 0 && ! (nullBitmap[j / 8] & 1 << j % 8))
                {
                    ociter->writeItem(_nullVal);
                }
                else
                {
                    _val.setDouble(arrayData[j]);
                    ociter->writeItem(_val);
                }
                ++valPos[2];
            }
            break;
        }
        case TE_STRING:
        {
            std::shared_ptr<arrow::StringArray> arrayString =
                std::static_pointer_cast<arrow::StringArray>(array);

            for(int64_t j = 0; j < numRows; ++j)
            {
                ociter->setPosition(valPos);
                if (nullCount != 0 && ! (nullBitmap[j / 8] & 1 << j % 8))
                {
                    ociter->writeItem(_nullVal);
                }
                else
                {
                    // Strings in Arrow arrays are not null-terminated
                    _val.setString(arrayString->GetString(j));
                    ociter->writeItem(_val);
                }
                ++valPos[2];
            }
            break;
        }
        case TE_BINARY:
        {
            std::shared_ptr<arrow::BinaryArray> arrayBinary =
                std::static_pointer_cast<arrow::BinaryArray>(array);

            for(int64_t j = 0; j < numRows; ++j)
            {
                ociter->setPosition(valPos);
                if (nullCount != 0 && ! (nullBitmap[j / 8] & 1 << j % 8))
                {
                    ociter->writeItem(_nullVal);
                }
                else
                {
                    const uint8_t* ptr_val;
                    int32_t sz_val;
                    ptr_val = arrayBinary->GetValue(j, &sz_val);
                    _val.setData(ptr_val, sz_val);
                    ociter->writeItem(_val);
                }
                ++valPos[2];
            }
            break;
        }
        default: throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL,
                                        SCIDB_LE_ILLEGAL_OPERATION)
            << "internal error: unknown type";
        }
        ociter->flush();
    }

    // populate the empty tag
    LOG4CXX_DEBUG(logger, "readFeather::empty tags...");
    Value bmVal;
    bmVal.setBool(true);
    shared_ptr<ChunkIterator> bmCiter = _oaiters[_nOutputAttrs]->newChunk(
        _outPos).getIterator(_query,
                             ChunkIterator::SEQUENTIAL_WRITE
                             | ChunkIterator::NO_EMPTY_CHECK);
    Coordinates valPos = _outPos;
    for(int64_t j =0; j<numRows; ++j)
    {
        bmCiter->setPosition(valPos);
        bmCiter->writeItem(bmVal);
        ++valPos[2];
    }
    bmCiter->flush();
    _outPos[1]++;
}

}}