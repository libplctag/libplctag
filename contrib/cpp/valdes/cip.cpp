/*****************************************************************
*                                                                *
*   C++ WRAPPER OF THE  LIBPLCTAG FOR CIP COMMUNICATION.         *
*                                                                *
*   THE CIP LIBRARY WAS MADE BY KYLE HAYES.                      *
*                                                                *
*   LINK:https://github.com/kyle-github/libplctag                *
*                                                                *
*   AUTHOR OF THE WRAPPER: Reyan Valdes   reyanvaldes@yahoo.com  *
*                    ITG:  www.itgtec.com                        *
*****************************************************************/

/***************************************************************
 CHANGE VERSIONS
 - 2015-02-03  RV First version

****************************************************************/

#include "cip.h"
#include <iostream>
#include <unistd.h> // for sleep

// Convert int Number to String
string intToStr (int vnumber)
{
 ostringstream convert;
 convert << vnumber;
 return convert.str();
};

// constructor to initialize all parameters
tcip::tcip()
{
 com.cpu =""; com.ip =""; com.protocol=""; com.portType =1; com.slot =0; com.timeout=3000;
 debug = false;
}

// Constructor to set Communication parameters related with CPU, which are common for all tags
//  protocol type: ab_eip, ab_cip
//  IP adress: 192.168.1.10
//  AB CPU models: plc,plc5,slc,slc500,micrologix,mlgx,compactlogix,clgx,lgx,controllogix,contrologix,flexlogix,flgx
//  Communication Port Type: 1- Backplane, 2- Control Net/Ethernet, DH+ Channel A, DH+ Channel B, 3- Serial
//  Slot number where cpu is installed: 0,1..
//  Timeout for reading/writing tags: 3000 ms
tcip::tcip ( string protocol, string ip, string cpu,
                int portType,  int slot, long timeout)
{
 SetCpuParam (protocol, ip, cpu, portType, slot, timeout);
 debug = false;
};


// The destructor release all memory and resources assigned to each tag before destroy object
tcip::~tcip()
{
 DelTagList();
}

// Set Communication parameters related with CPU, which are common for all tags, for example:
//  protocol type: ab_eip, ab_cip
//  IP adress: 192.168.1.10
//  AB CPU models: plc,plc5,slc,slc500,micrologix,mlgx,compactlogix,clgx,lgx,controllogix,contrologix,flexlogix,flgx
//  Communication Port Type: 1- Backplane, 2- Control Net/Ethernet, DH+ Channel A, DH+ Channel B, 3- Serial
//  Slot number where cpu is installed: 0,1..
//  Time out for reading/writing tags: 3000 ms
void tcip::SetCpuParam (    string protocol, string ip, string cpu,
                               int portType,  int slot, long timeout)
{
 com.protocol = protocol; com.ip = ip; com.cpu = cpu;
 com.portType = portType; com.slot = slot;
 com.timeout  = timeout;
};

 // Get Communication path in a format compatible with the CIP library (only until cpu parameter)
 // TAG_PATH= "protocol=ab_eip&gateway=192.168.1.207&path=1,0&cpu=LGX&elem_count=1&elem_size=4&name=Real"
 string tcip::GetCpuPath ()
 {
  return "protocol=" + com.protocol + "&gateway=" + com.ip + "&path=" + intToStr(com.portType) + "," + intToStr(com.slot) + "&cpu=" + com.cpu;
 };

 // Get Data Type Size in bytes
//  1- BOOL = 1
//  2- SINT = 1
//  3- INT  = 2
//  4- DINT = 4
//  5- REAL = 4

 int tcip::GetTypeSize( int type)
 {
  int result;
  switch (type)
  {
   case 1: // for BOOL
         result = 1;
         break;
   case 2: // for SINT
         result = 1;
         break;
   case 3: // for INT
         result = 2;
         break;
   case 4: // for DINT
         result = 4;
         break;
   case 5: // for REAL
         result = 4;
         break;
   default:
         result = 0;
  }; // switch end
  return result;
 };

// Get completely tag path which include cpu communication parameters + tag parameters itself
// TAG_PATH= "protocol=ab_eip&gateway=192.168.1.207&path=1,0&cpu=LGX&elem_count=1&elem_size=4&name=Real"
 string tcip::GetTagPath(const tCIPTagParam & tag)
 {
  string str = GetCpuPath() + "&elem_count=" + intToStr(tag.elemCount) + "&elem_size=" + intToStr(GetTypeSize(tag.tagType)) + "&name=" + tag.tagName;
  if (debug)
   str += "&debug=1"; // add the debug parameters if debug is active. The debug is set using SetDebug
  return str;
 };

 // Get Tags List
tCIPTagList tcip::GetTagList ()
{
 return tags; // return tags list
};

// Get Tag Status
 int tcip::GetTagStatus (int tagNo)
 {
  return plc_tag_status(tags[tagNo].tagPtr);
 };


 // Add tag in the list and create pointer to the plc tag
 // It assume the SetCommParam method was called before to set all communication parameters
 // return: index of the tag in the list, or -1 if had a problem

 int tcip::AddTag ( string name, int type, int elemCount)
 {
   // Set Tag parameters
  tCIPTagParam tag;
  // check limit to 1 element count
  if (elemCount==0)
   elemCount =1;

  // set all parameters
  tag.tagName = name; tag.tagType = type; tag.elemCount = elemCount; tag.tagPtr = PLC_TAG_NULL;

  string tagPath = GetTagPath (tag); // get string path

 // Create tag in CIP and check for status
  tag.tagPtr = plc_tag_create (tagPath.c_str());

   if(!tag.tagPtr) {
        cout <<"ERROR: Could not create tag" << endl;
        return -1;
    }

    // let the connect succeed
    while(plc_tag_status(tag.tagPtr) == PLCTAG_STATUS_PENDING) {
    	sleep(1);
    }

    if(plc_tag_status(tag.tagPtr) != PLCTAG_STATUS_OK) {
    	cout << "Error setting up tag internal state" << endl;
    	return -1;
    }

  // add at the end of the list (vector)
  tags.push_back(tag);

  // return index (last one in the list) = size -1 (starting in 0 index)
  return tags.size()-1;
 };

 // Read Tag from the PLC into the CIP buffer, after has to apply get to get the value
 // return error code
 int tcip::ReadTag ( int tagNo)
 {
  if ((tagNo <0) || (tagNo>=tags.size())) // check limits
   return PLCTAG_ERR_OUT_OF_BOUNDS;
  else {
   // get the data
   int  rc = plc_tag_read(tags[tagNo].tagPtr, com.timeout); // read tag into CPI buffer
     if(rc != PLCTAG_STATUS_OK) {
       cout << "ERROR: Unable to read the data! Got error code " << rc << endl;
     }; // if not status ok - end
    return rc;
   };
 };

 // Write Tag from the CIP buffer into the PLC, before had to apply set to set the value
 // return error code
int tcip::WriteTag ( int tagNo)
{
  if ((tagNo <0) || (tagNo>=tags.size())) // check limits
   return PLCTAG_ERR_OUT_OF_BOUNDS;
  else {
   int rc = plc_tag_write(tags[tagNo].tagPtr, com.timeout);

    if(rc != PLCTAG_STATUS_OK) {
        cout << "ERROR: Unable to read the data! Got error code " << rc << endl;
     };
   return rc;
  };
};

 // Get real tag value from buffer, had to call ReadTag first
 // offset is in bytes, basically used for arrays. For example:
 // if need to read index of the array, offset = index * element size
float tcip::GetTagReal ( int tagNo, int offset )
{
  return plc_tag_get_float32(tags[tagNo].tagPtr, offset ); // return float 32
};

// Get dint tag value from buffer, had to call ReadTag first
// offset is in bytes, basically used for arrays. For example:
// if need to read index of the array, offset = index * element size
 long tcip::GetTagDint ( int tagNo, int offset )
 {
  return plc_tag_get_int32(tags[tagNo].tagPtr, offset); // return int 32
 };

 // Get int tag value from buffer, had to call ReadTag first
 // offset is in bytes, basically used for arrays. For example:
 // if need to read index of the array, offset = index * element size
 int tcip::GetTagInt  ( int tagNo, int offset )
 {
   return plc_tag_get_int16(tags[tagNo].tagPtr, offset); // return int 16
 };

// Get sint tag value from buffer, had to call ReadTag first
// offset is in bytes, basically used for arrays. For example:
// if need to read index of the array, offset = index * element size
int  tcip::GetTagSint ( int tagNo, int offset )
{
  int result = plc_tag_get_int8 (tags[tagNo].tagPtr, offset); // get value int 8 and transfer to int to allow cout on the screen directly if needed without problem
  return result;
};

// Get Bool tag value from buffer, had to call ReadTag first
// offset is in bytes, basically used for arrays. For example:
// if need to read index of the array, offset = index * element size
bool tcip::GetTagBool ( int tagNo, int offset )
{
  return GetTagSint(tagNo, offset)== -1;
};

// Set real tag value into buffer, have to call WriteTag after
// offset is in bytes, basically used for arrays. For example:
// if need to set index of the array, offset = index * element size
void tcip::SetTagReal ( int tagNo, int offset, float value )
{
  plc_tag_set_float32 (tags[tagNo].tagPtr,offset,value);
};

 // Set dint tag value into buffer, have to call WriteTag after
 // offset is in bytes, basically used for arrays. For example:
// if need to set index of the array, offset = index * element size
void tcip::SetTagDint ( int tagNo, int offset, long value )
{
  plc_tag_set_int32(tags[tagNo].tagPtr,offset,value);
};

// Set int tag value into buffer, have to call WriteTag after
// offset is in bytes, basically used for arrays. For example:
// if need to set index of the array, offset = index * element size
void tcip::SetTagInt ( int tagNo, int offset, int value )
{
  plc_tag_set_int16(tags[tagNo].tagPtr,offset,value);
};

// Set sint tag value into buffer, have to call WriteTag after
// offset is in bytes, basically used for arrays. For example:
// if need to set index of the array, offset = index * element size
void tcip::SetTagSint  ( int tagNo, int offset, int8_t value )
{
 plc_tag_set_int8(tags[tagNo].tagPtr,offset,value);
};

// Set bool tag value into buffer, have to call WriteTag after
// offset is in bytes, basically used for arrays. For example:
// if need to set index of the array, offset = index * element size
void tcip::SetTagBool  ( int tagNo, int offset, bool value )
{
 int8_t plc_value;

 if (value)
  plc_value = -1;
 else
  plc_value = 0;

 plc_tag_set_int8(tags[tagNo].tagPtr,offset,plc_value);
};

// Delete tag number from list and release its resources
 int tcip::DelTagNo ( int tagNo)
 {
  // Check index
  if ((tagNo >=0) && (tagNo< tags.size()))  { // check limits (0-size -1)
    if ( tags[tagNo].tagPtr  !=PLC_TAG_NULL ) {  // if it is not null, then destroy
     return plc_tag_destroy (tags[tagNo].tagPtr); // destroy tag
   };
  };

  return PLCTAG_STATUS_OK;
 };

  // Delete all tags list
 int tcip::DelTagList ()
 {
  for (short int i=0; i< tags.size(); i++)
   {
      DelTagNo (i); // Delete and release resources from tag n (0..n-1)
   };
  tags.clear(); // clear the vector

  return PLCTAG_STATUS_OK;
 };

// Lock the tag against use by other threads
int  tcip::LockTag   ( int tagNo)
{
 return plc_tag_lock( tags[tagNo].tagPtr );
};

// Unlock the tag.The opposite action of LockTag
int  tcip::UnlockTag ( int tagNo)
{
 return plc_tag_unlock( tags[tagNo].tagPtr );
};

 //Find tag index into the tags List. Return -1 if tag not found
int tcip::FindTag (string name)
{
 for (int i=0; i < tags.size(); i++)
 {
  if (tags[i].tagName == name) {
   return i; // return index
  };
 }; // for end
 return -1; // name not found
};

// Return tags count inserted in the list
int tcip::GetTagsCount ()
{
 return tags.size();
};


// Set debug mode (true/false) to allow print messages for debugging, by default it is false
// Has to be called at beginning before adding any tag to create the tag path with the debugging option
// once it is set, it can be reset but need to delete all tags and adding again.
// By default it is false, so there is no need to call this method
void tcip::SetDebug (bool active)
{
 debug = active; // set debug mode
};
