#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string>
#include <sstream>
#include <iostream>

using std::string;
using std::ostringstream;
using std::cout;
using std::endl;

enum ExecutionMode
{
    NORMAL      = 0,
    READ_DELAY  = 1,
    WRITE_DELAY = 2,
    SUMMARIZE   = 3
};

int basicLoop(ExecutionMode mode)
{
    char* line = NULL;
    size_t len = 0;
    int read = getline(&line, &len, stdin);
    while(read > 0)
    {
        char * end = line;
        errno = 0;
        int nLines = strtoll(line, &end, 10);
        if(errno!=0 || (*end) != '\n')
        {
            return 1;
        }
        if (nLines == 0)
        {
            cout<<0<<std::endl;
            cout<<std::flush;
            return 0;
        }
        ostringstream output;
        output<<nLines+1<<"\n";
        size_t i;
        for( i =0 ; i<nLines; ++i)
        {
            if(mode == READ_DELAY)
            {
                sleep(10000);
            }
            read = getline(&line, &len, stdin);
            if(read <= 0)
            {
                return 1;
            }
            output<<"Hello\t"<<line;
        }
        output<<"OK\tthanks!\n";
        string outputString = output.str();
        string firstPacket = outputString.substr(0,20);
        cout<<firstPacket;
        cout<<std::flush;
        if(outputString.size()>20)
        {
            if(mode == WRITE_DELAY)
            {
                sleep(10000);
            }
            string secondPacket = outputString.substr(20,outputString.length());
            cout<<secondPacket;
            cout<<std::flush;
        }
        read = getline(&line, &len, stdin);
    }
    free(line);
    return 0;
}

int summarizeLoop()
{
    char* line = NULL;
    size_t len = 0;
    int read = getline(&line, &len, stdin);
    size_t totalLines = 0;
    while(read > 0)
    {
        char * end = line;
        errno = 0;
        int nLines = strtoll(line, &end, 10);
        if(errno!=0 || (*end) != '\n')
        {
            return 1;
        }
        if (nLines == 0)
        {
            cout<<"1\n";
            cout<<"Thanks! That was a total of "<<totalLines<<" lines.\n";
            cout<<std::flush;
            return 0;
        }
        for(int i =0 ; i<nLines; ++i)
        {
            read = getline(&line, &len, stdin);
            if(read <= 0)
            {
                return 1;
            }
            ++totalLines;
        }
        cout<<"0\n";
        cout<<std::flush;
        read = getline(&line, &len, stdin);
    }
    free(line);
    return 0;
}


int main(int argc, char* argv[])
{
    ExecutionMode mode = NORMAL;
    if(argc==2)
    {
        string modeString(argv[1]);
        if(modeString == "NORMAL")
        {
            return basicLoop(NORMAL);
        }
        else if (modeString == "READ_DELAY")
        {
            return basicLoop(READ_DELAY);
        }
        else if (modeString == "WRITE_DELAY")
        {
            return basicLoop(WRITE_DELAY);
        }
        else if (modeString == "SUMMARIZE")
        {
            return summarizeLoop();
        }
        else
        {
            std::cerr<<"Unknown mode "<<modeString<<std::endl;
            abort();
        }
    }
    return basicLoop(NORMAL);
}
