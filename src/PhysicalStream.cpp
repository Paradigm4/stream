/*
**
* BEGIN_COPYRIGHT
*
* Copyright (C) 2008-2016 SciDB, Inc.
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

#include <limits>
#include <sstream>
#include <memory>
#include <string>
#include <vector>
#include <ctype.h>
#include <query/TypeSystem.h>
#include <query/Operator.h>
#include <log4cxx/logger.h>
#include <sys/wait.h>
#include "../lib/slave.h"
#include "../lib/serial.h"

using std::shared_ptr;
using std::make_shared;
using std::string;
using std::ostringstream;
using std::vector;

namespace scidb
{

namespace stream
{

// Logger for operator. static to prevent visibility of variable outside of file
static log4cxx::LoggerPtr logger(log4cxx::Logger::getLogger("scidb.operators.stream"));

/**
 * A class that writes an output array given a sequence of strings
 */
class OutputWriter : public boost::noncopyable
{
private:
    shared_ptr<Array>           _output;
    InstanceID const            _myInstanceId;
    shared_ptr<Query>           _query;
    size_t                      _chunkNo;
    shared_ptr<ArrayIterator>   _arrayIter;

public:
    /**
     * Create from a schema and query context.
     * @param schema must match (2 dims, 1 string att)
     * @param query the query context
     */
    OutputWriter(ArrayDesc const& schema, shared_ptr<Query>& query):
        _output(new MemArray(schema, query)),
        _myInstanceId(query->getInstanceID()),
        _query(query),
        _chunkNo(0),
        _arrayIter(_output->getIterator(0))
    {}

    /**
     * Write the given string into a new chunk of the array
     */
    void writeString(string const& str)
    {
        if(_output.get() == NULL)
        {
            throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION) << "stream::OuputWriter::writeString called on an invalid object";
        }
        Coordinates pos(2);
        pos[0] = _myInstanceId;
        pos[1] = _chunkNo;
        shared_ptr<ChunkIterator> chunkIter = _arrayIter->newChunk(pos).getIterator(_query, ChunkIterator::SEQUENTIAL_WRITE);
        chunkIter->setPosition(pos);
        Value result;
        result.setString(str);
        chunkIter->writeItem(result);
        chunkIter->flush();
        _chunkNo++;
    }

    /**
     * Retrieve the final array. After this call, this object is invalid.
     * @return the array containing data from all the previous writeString calls
     */
    shared_ptr<Array> finalize()
    {
        _arrayIter.reset();
        shared_ptr<Array> res = _output;
        _output.reset();
        _query.reset();
        return res;
    }
};

/**
 * An abstraction over the slave process forked by SciDB.
 */
class SlaveProcess
{
private:
    slave _slaveContext;
    FILE *_slaveOutput;
    bool _alive;

public:
    /**
     * Fork a new process.
     * @param commandLine a single executable file at the moment. Just put it all in a script, bro.
     */
    SlaveProcess(string const& commandLine):
        _alive(false)
    {
        string commandLineCopy = commandLine;
        char* argv[2];
        argv[0] = const_cast<char*>(commandLine.c_str());
        argv[1] = NULL;
        _slaveContext = run (argv, NULL, NULL);
        if (_slaveContext.pid < 0)
        {
            throw SYSTEM_EXCEPTION(SCIDB_SE_INTERNAL, SCIDB_LE_ILLEGAL_OPERATION) << "fork failed, bummer";
        }
        _alive = true;
        _slaveOutput = fdopen (_slaveContext.out, "r");
        if(_slaveOutput == NULL)
        {
            LOG4CXX_DEBUG(logger, "STREAM: slave terminated early: fopen");
            terminate();
        }
    }

    ~SlaveProcess()
    {
        terminate();
    }

    /**
     * @return true if the child is alive and healthy as far as we know; false otherwise
     */
    bool isAlive()
    {
        return _alive;
    }

    bool tsvExchange(size_t const nLines, char const* inputData, string& outputData)
    {
        ssize_t ret = write_tsv(_slaveContext.in, inputData, nLines);
        if(ret<0)
        {
            LOG4CXX_DEBUG(logger, "STREAM: slave terminated early: write_tsv");
            terminate();
            return false;
        }
        // "f[orget] them new model cars, we ridin old skool!"
        class auto_free_line
        {
        public:
           char *text;
           auto_free_line(): text(NULL) {}
           ~auto_free_line() { ::free(text); }
        };
        auto_free_line line;
        size_t lineLen = 0;
        //TODO: HOLE - if the slave never responds, we are in trouble!
        if (getline (&line.text, &lineLen, _slaveOutput) < 0)
        {
            LOG4CXX_DEBUG(logger, "STREAM: slave terminated early: getline (header)");
            terminate();
            return false;
        }
        LOG4CXX_DEBUG(logger, "STREAM: got line "<<line.text);
        errno = 0;
        char* end = line.text;
        int64_t nOutputLines = strtoll(line.text, &end, 10);
        if(errno != 0 || *end != '\n' || nOutputLines < 0)
        {
            LOG4CXX_DEBUG(logger, "STREAM: slave terminated early: header not valid");
            terminate();
            return false;
        }
        LOG4CXX_DEBUG(logger, "STREAM: out lines "<<nOutputLines);
        ostringstream output;
        for (int64_t j = 0; j < nOutputLines; ++j)
        {
            if (getline (&line.text, &lineLen, _slaveOutput) < 0)
            {
                LOG4CXX_DEBUG(logger, "STREAM: slave terminated early: getline");
                terminate();
                return false;
            }
            output<<line.text;
        }
        outputData = output.str();
        return true;
    }

    void terminate()
    {
        if(_alive)
        {
            close (_slaveContext.in);
            close (_slaveContext.out);
            kill (_slaveContext.pid, SIGTERM);
            int status;
            waitpid (_slaveContext.pid, &status, WNOHANG);
            if (!WIFEXITED (status))
            {
                kill (_slaveContext.pid, SIGKILL);
                waitpid (_slaveContext.pid, &status, WNOHANG);
            }
            ::fclose(_slaveOutput);
            _alive = false;
        }
    }
};


class TextChunkConverter
{
private:
    enum AttType
    {
        OTHER   =0,
        STRING =1,
        FLOAT  =2,
        DOUBLE =3,
        BOOL   =4,
        UINT8  =5,
        INT8    =6
    };

    char const              _attDelim;
    char const              _lineDelim;
    bool const              _printCoords;
    bool const              _quoteStrings;
    size_t const            _precision;
    size_t const            _nAtts;
    vector<AttType>         _attTypes;
    vector<FunctionPointer> _converters;
    Value                   _stringBuf;
    string                  _nanRepresentation;
    string                  _nullRepresentation;

public:
    TextChunkConverter(ArrayDesc const& inputArrayDesc):
       _attDelim(  '\t'),
       _lineDelim( '\n'),
       _printCoords(false),
       _quoteStrings(true),
       _precision(10),
       _nAtts(inputArrayDesc.getAttributes(true).size()),
       _attTypes(_nAtts, OTHER),
       _converters(_nAtts, 0),
       _nanRepresentation("nan"),
       _nullRepresentation("null")
    {
        Attributes const& inputAttrs = inputArrayDesc.getAttributes(true);
        for (size_t i = 0; i < inputAttrs.size(); ++i)
        {
            if(inputAttrs[i].getType() == TID_STRING)
            {
                _attTypes[i] = STRING;
            }
            else if(inputAttrs[i].getType() == TID_BOOL)
            {
                _attTypes[i] = BOOL;
            }
            else if(inputAttrs[i].getType() == TID_DOUBLE)
            {
                _attTypes[i] = DOUBLE;
            }
            else if(inputAttrs[i].getType() == TID_FLOAT)
            {
                _attTypes[i] = FLOAT;
            }
            else if(inputAttrs[i].getType() == TID_UINT8)
            {
                _attTypes[i] = UINT8;
            }
            else if(inputAttrs[i].getType() == TID_INT8)
            {
                _attTypes[i] = INT8;
            }
            else
            {
                _converters[i] = FunctionLibrary::getInstance()->findConverter(
                    inputAttrs[i].getType(),
                    TID_STRING,
                    false);
            }
        }
    }

    ~TextChunkConverter()
    {}

    void convertChunk(vector<shared_ptr<ConstChunkIterator> > citers, size_t &nCells, string& output)
    {
        nCells = 0;
        ostringstream outputBuf;
        outputBuf.precision(_precision);
        while(!citers[0]->end())
        {
            if(_printCoords)
            {
                Coordinates const& pos = citers[0]->getPosition();
                for(size_t i =0, n=pos.size(); i<n; ++i)
                {
                    if(i)
                    {
                        outputBuf<<_attDelim;
                    }
                    outputBuf<<pos[i];
                }
            }
            for (size_t i = 0; i < _nAtts; ++i)
            {
                Value const& v = citers[i]->getItem();
                if (i || _printCoords)
                {
                    outputBuf<<_attDelim;
                }
                if(v.isNull())
                {
                    outputBuf<<_nullRepresentation; //TODO: all missing codes are converted to this representation
                }
                else
                {
                    switch(_attTypes[i])
                    {
                    case STRING:
                        if(_quoteStrings)
                        {
                            char const* s = v.getString();
                            outputBuf << '\'';
                            while (char c = *s++)
                            {
                                if (c == '\'')
                                {
                                    outputBuf << '\\' << c;
                                }
                                else if (c == '\\')
                                {
                                    outputBuf << "\\\\";
                                }
                                else
                                {
                                    outputBuf << c;
                                }
                            }
                            outputBuf << '\'';
                        }
                        else
                        {
                            outputBuf<<v.getString();
                        }
                        break;
                    case BOOL:
                        if(v.getBool())
                        {
                            outputBuf<<"true";
                        }
                        else
                        {
                            outputBuf<<"false";
                        }
                        break;
                    case DOUBLE:
                        {
                            double nbr =v.getDouble();
                            if(std::isnan(nbr))
                            {
                                outputBuf<<_nanRepresentation;
                            }
                            else
                            {
                                outputBuf<<nbr;
                            }
                        }
                        break;
                    case FLOAT:
                        {
                            float fnbr =v.getFloat();
                            if(std::isnan(fnbr))
                            {
                                outputBuf<<_nanRepresentation;
                            }
                            else
                            {
                                outputBuf<<fnbr;
                            }
                        }
                        break;
                    case UINT8:
                        {
                            uint8_t nbr =v.getUint8();
                            outputBuf<<(int16_t) nbr;
                        }
                        break;
                    case INT8:
                        {
                            int8_t nbr =v.getUint8();
                            outputBuf<<(int16_t) nbr;
                        }
                        break;
                    case OTHER:
                        {
                            Value const * vv = &v;
                            (*_converters[i])(&vv, &_stringBuf, NULL);
                            outputBuf<<_stringBuf.getString();
                        }
                    }
                }
            }
            outputBuf<<_lineDelim;
            ++nCells;
            for(size_t i = 0; i<_nAtts; ++i)
            {
                ++(*citers[i]);
            }
        }
        output = outputBuf.str();
    }
};

}

using namespace stream;

class PhysicalStream : public PhysicalOperator
{
public:
    PhysicalStream(std::string const& logicalName,
        std::string const& physicalName,
        Parameters const& parameters,
        ArrayDesc const& schema):
            PhysicalOperator(logicalName, physicalName, parameters, schema)
{}

std::shared_ptr< Array> execute(std::vector< std::shared_ptr< Array> >& inputArrays, std::shared_ptr<Query> query)
{
    shared_ptr<Array>& inputArray = inputArrays[0];
    ArrayDesc const& inputSchema = inputArray ->getArrayDesc();
    size_t const nAttrs = inputSchema.getAttributes(true).size();
    vector <shared_ptr<ConstArrayIterator> > aiters (nAttrs);
    for(size_t i =0; i<nAttrs; ++i)
    {
        aiters[i] = inputArray->getConstIterator(i);
    }
    vector <shared_ptr<ConstChunkIterator> > citers (nAttrs);
    TextChunkConverter converter(inputSchema);
    OutputWriter outputWriter(_schema, query);
    SlaveProcess slave("/home/apoliakov/streaming/src/client");
    bool slaveAlive = slave.isAlive();
    string tsvInput;
    string output;
    size_t nCells=0;
    while(!aiters[0]->end() && slaveAlive)
    {
        for(size_t i =0; i<nAttrs; ++i)
        {
            citers[i] = aiters[i]->getChunk().getConstIterator(ConstChunkIterator::IGNORE_OVERLAPS | ConstChunkIterator::IGNORE_EMPTY_CELLS);
        }
        converter.convertChunk(citers, nCells, tsvInput);
        slaveAlive = slave.tsvExchange(nCells, tsvInput.c_str(), output);
        if(slaveAlive)
        {
            outputWriter.writeString(output);
        }
        for(size_t i =0; i<nAttrs; ++i)
        {
           ++(*aiters[i]);
        }
    }
    slave.terminate();
    return outputWriter.finalize();
}
};

REGISTER_PHYSICAL_OPERATOR_FACTORY(PhysicalStream, "stream", "PhysicalStream");

} // end namespace scidb