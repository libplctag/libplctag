/*****************************************************************
*                                                                *
*   C++ WRAPPER OF THE LIBPLCTAG FOR CIP COMMUNICATION.          *
*                                                                *
*   THE CIP LIBRARY WAS MADE BY KYLE HAYES.                      *
*                                                                *
*   LINK:https://github.com/kyle-github/libplctag                *
*                                                                *
*   AUTHOR OF THE WRAPPER: Reyan Valdes   reyanvaldes@yahoo.com  *
*                     ITG: www.itgtec.com                        *
*****************************************************************/

/***************************************************************
 CHANGE VERSIONS
 - 2015-02-03  RV First version

****************************************************************/


#ifndef CIP_H
#define CIP_H

#include <string>
#include <vector>
#include <sstream>

using namespace std;

// Uses the libplctag, has to link with external C option to allow linking correctly in C++
extern "C" {
#include "libplctag.h"
}

// Define different data types
#define CIP_DATA_TYPE_BOOL 1
#define CIP_DATA_TYPE_SINT 2
#define CIP_DATA_TYPE_INT  3
#define CIP_DATA_TYPE_DINT 4
#define CIP_DATA_TYPE_REAL 5


// tCIPCPUParam
struct  tCIPCPUParam {   // CPI communication parameters
         string protocol; // protocol type: ab_eip, ab_cip
         string ip;       // IP address: 192.168.1.10
         string cpu;      // AB CPU model: plc,plc5,slc,slc500,micrologix,mlgx,compactlogix,clgx,lgx,controllogix,contrologix,flexlogix,flgx
         int    portType; // Communication Port Type: 1- Backplane, 2- Control Net/Ethernet, DH+ Channel A, DH+ Channel B, 3- Serial
         int    slot;     // Slot number where cpu is installed
         long   timeout;  // Time out for reading/writing tags
};

// Tag Parameters
struct  tCIPTagParam {     // Tag parameter
         string tagName;   // tag name
           int  tagType;   // Tag type. As defined on CIP_DATA_TYPE_XXXX
           int  elemCount; // elements count: 1- single, n-array
        plc_tag tagPtr;    // PLC Tag pointer. If null means wasn't assigned
};

// Tag list
typedef vector <tCIPTagParam> tCIPTagList; // Vector of tCIPTagParam


// CIP class type
class tcip
{
    public:
               tcip();                                                   // constructor to initialize with default for all communication parameters
               tcip ( string protocol, string ip, string cpu,
                  int portType, int slot, long timeout);                 // Constructor to set Communication parameters related with CPU, which are common for all tags

        virtual ~tcip();                                                 // destructor, release all memory and resources assigned to each tag

          void SetCpuParam (  string protocol, string ip, string cpu,
                                 int portType,  int slot, long timeout); // Set Communication parameters related with CPU, which are common for all tags
        string GetTagPath (const tCIPTagParam & tag);                    // Get completely tag path which include cpu communication parameters + tag parameters itself

           int AddTag ( string name, int type, int elemCount);           // Add tag in the list and create pointer to the plc tag
           int ReadTag (int tagNo);                                      // Read Tag from PLC into the CIP buffer, after has to apply get to get the value
           int WriteTag (int tagNo);                                     // Write Tag from the CIP buffer into PLC, before had to applied set to set the value

         float GetTagReal ( int tagNo, int offset );                      // Get real tag value from buffer, had to call ReadTag first
          long GetTagDint ( int tagNo, int offset );                      // Get dint tag value from buffer, had to call ReadTag first
          int  GetTagInt  ( int tagNo, int offset );                      // Get int tag value from buffer, had to call ReadTag first
          int  GetTagSint ( int tagNo, int offset );                      // Get sint tag value from buffer, had to call ReadTag first. It return int to allow easily print it
          bool GetTagBool ( int tagNo, int offset );                      // Get Bool tag value from buffer, had to call ReadTag first

          void SetTagReal ( int tagNo, int offset, float value );         // Set real tag value into buffer, have to call WriteTag after
          void SetTagDint ( int tagNo, int offset, long value );          // Set dint tag value into buffer, have to call WriteTag after
          void SetTagInt  ( int tagNo, int offset, int value );           // Set int tag value into buffer, have to call WriteTag after
          void SetTagSint ( int tagNo, int offset, int8_t value );        // Set sint tag value into buffer, have to call WriteTag after
          void SetTagBool ( int tagNo, int offset, bool value );          // Set bool tag value into buffer, have to call WriteTag after

          int  LockTag   ( int tagNo);                                    // Lock the tag against use by other threads
          int  UnlockTag ( int tagNo);                                    // Unlock the tag.The opposite action of LockTag

           int DelTagNo ( int tagNo);                                     // Release the tag resources
           int DelTagList ();                                             // Delete completely tags list

           int FindTag (string name);                                     // Find tag index into the tags List. Return -1 if tag not found
           int GetTypeSize( int type);                                    // Get Data Type Size
   tCIPTagList GetTagList ();                                             // Get Tags List
           int GetTagStatus (int tagNo);                                  // Get Tag Status
           int GetTagsCount ();                                           // Return tags count inserted in the list

           void SetDebug (bool active);                                   // Set debug mode (true/false) to allow print messages for debugging, by default it is false

    protected:
       tCIPCPUParam com;     // CIP communication parameters
       tCIPTagList  tags;    // CIP Tags list (vector)
               bool debug;   // Control debug mode, this will allow print debug messages. It can be true with SetDebug
    private:
        string GetCpuPath(); // Get CPU Communication path in a compatible format with the CIP library
};

#endif // CIP_H
