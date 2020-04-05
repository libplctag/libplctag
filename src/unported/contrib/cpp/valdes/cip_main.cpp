// Example program of the CIP class
//
// Company: ITG
// Author: Reyan V.

// Date: 2015.03.03

#include <iostream>
extern "C" {
#include "libplctag2.h"
}
#include <cip.h>

using namespace std;

/*
 In this example was used Logix with IP = 192.168.1.207, located at backplane in the slot 11
 The tags created were:
 - Tag Name      Data Type         Value
 ---------------------------------------
    Real           REAL            10.5
    Dint           DINT            32
    Int            INT             20
    Sint           SINT            14
    Bool           BOOL            true
    Array          INT[8]          1,2,...,8
*/

// Testing CIP wrapper of the libplctag library
void testingCIP()
{
// uses protocol ab_eip and cpu=Logix with ip=192.168.1.207 located at the backplane (1), in the slot 0, with time out = 3000 for all tag reading
 tcip cip ("ab_eip","192.168.1.207","LGX",1,0,3000);

 cip.SetDebug (false); // Just needed if want set debug mode to true to see debugging messages. By default debug mode = false, so it is no needed to call SetDebug

 int tag[6];
 // Add all tags links
 tag[0] = cip.AddTag ("Real", CIP_DATA_TYPE_REAL,1);
 tag[1] = cip.AddTag ("Dint", CIP_DATA_TYPE_DINT,1);
 tag[2] = cip.AddTag ("Int" , CIP_DATA_TYPE_INT, 1);
 tag[3] = cip.AddTag ("Sint", CIP_DATA_TYPE_SINT,1);
 tag[4] = cip.AddTag ("Bool", CIP_DATA_TYPE_BOOL,1);
 tag[5] = cip.AddTag ("Array",CIP_DATA_TYPE_INT, 8);

// Write tags values
cip.SetTagReal(tag[0], 0, 10.5); cip.WriteTag (tag[0]);
cip.SetTagDint(tag[1], 0, 32);   cip.WriteTag (tag[1]);
cip.SetTagInt (tag[2], 0, 20);   cip.WriteTag (tag[2]);
cip.SetTagSint(tag[3], 0, 14);   cip.WriteTag (tag[3]);
cip.SetTagBool (tag[4],0, true); cip.WriteTag (tag[4]);

// Set array elements with values 1..8
for (int i=0; i< 8 ; i++)
 cip.SetTagInt (tag[5],i * cip.GetTypeSize(CIP_DATA_TYPE_INT), i+1); // compute offset i * 2

// Update Array
cip.WriteTag (tag[5]);

// Another way to write all tags in loop
for (int i=0; i< cip.GetTagsCount(); i++)
 cip.WriteTag (tag[i]);  // In this case because tag[i] has same value that i, the equivalent instruction would be cip.WriteTag (i)

// read all tags
 for (int i=0; i< cip.GetTagsCount() ;i++) {
   cip.ReadTag(tag[i]); // In this case because tag[i] has same value that i, the equivalent instruction would be cip.ReadTag (i)
  };


 //Show current tag values
 cout << " Tag: " << tag[0] << " Value: " << cip.GetTagReal(tag[0],0) << endl;
 cout << " Tag: " << tag[1] << " Value: " << cip.GetTagDint(tag[1],0) << endl;
 cout << " Tag: " << tag[2] << " Value: " << cip.GetTagInt( tag[2],0) << endl;
 cout << " Tag: " << tag[3] << " Value: " << cip.GetTagSint(tag[3],0) << endl;
 cout << " Tag: " << tag[4] << " Value: " << cip.GetTagBool(tag[4],0) << endl;

 // Show Array elements
 cout << " Tag: " << tag[5] << " Array Values " << endl;
 for (int i=0; i<8; i++)
  cout << i << "  Value " << cip.GetTagInt(tag[5], 2 * i) << endl;


 // The tags are destroy automatically when the cip object calls the destructor
 // no needed to call cip.DelTagList();
 };


int main(int argc, char *argv[])
{
 testingCIP();
 return 0;
}

