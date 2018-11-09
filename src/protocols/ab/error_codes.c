/***************************************************************************
 *   Copyright (C) 2016 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <platform.h>
#include <lib/libplctag.h>
#include <ab/error_codes.h>
#include <util/debug.h>


struct error_code_entry {
    int primary_code;
    int secondary_code;
    int translated_code;
    const char *short_desc;
    const char *long_desc;
};


/*
 * This information was constructed after finding a few online resources.  Most of it comes from publically published manuals for other products.
 * Sources include:
 * 	Kepware
 *  aboutplcs.com (Productivity 3000 manual)
 *  Allen-Bradley
 *  and others I have long since lost track of.
 *
 * Most probably comes from aboutplcs.com.
 *
 * The copyright on these entries that of their respective owners.  Used here under assumption of Fair Use.
 */


static struct error_code_entry error_code_table[] = {
    {0x01, 0x0100, PLCTAG_ERR_DUPLICATE, "Connection In Use/Duplicate Forward Open", "A connection is already established from the target device sending a Forward Open request or the target device has sent multiple forward open request. This could be caused by poor network traffic. Check the cabling, switches and connections."},
    {0x01, 0x0103, PLCTAG_ERR_UNSUPPORTED, "Transport Class/Trigger Combination not supported", "The Transport class and trigger combination is not supported. The Productivity Suite CPU only supports Class 1 and Class 3 transports and triggers: Change of State and Cyclic."},
    {0x01, 0x0106, PLCTAG_ERR_NOT_ALLOWED, "Owner Conflict", "An existing exclusive owner has already configured a connection to this Connection Point. Check to see if other Scanner devices are connected to this adapter or verify that Multicast is supported by adapter device if Multicast is selected for Forward Open. This could be caused by poor network traffic. Check the cabling, switches and connections."},
    {0x01, 0x0107, PLCTAG_ERR_NOT_FOUND, "Target Connection Not Found", "This occurs if a device sends a Forward Close on a connection and the device can't find this connection. This could occur if one of these devices has powered down or if the connection timed out on a bad connection. This could be caused by poor network traffic. Check the cabling, switches and connections."},
    {0x01, 0x0108, PLCTAG_ERR_BAD_PARAM, "Invalid Network Connection Parameter", "This error occurs when one of the parameters specified in the Forward Open message is not supported such as Connection Point, Connection type, Connection priority, redundant owner or exclusive owner. The Productivity Suite CPU does not return this error and will instead use errors 0x0120, 0x0121, 0x0122, 0x0123, 0x0124, 0x0125 or 0x0132 instead."},
    {0x01, 0x0109, PLCTAG_ERR_BAD_PARAM, "Invalid Connection Size", "This error occurs when the target device doesn't support the requested connection size. Check the documentation of the manufacturer's device to verify the correct Connection size required by the device. Note that most devices specify this value in terms of bytes. The Productivity Suite CPU does not return this error and will instead use errors 0x0126, 0x0127 and 0x0128."},
    {0x01, 0x0110, PLCTAG_ERR_NOT_FOUND, "Target for Connection Not Configured", "This error occurs when a message is received with a connection number that does not exist in the target device. This could occur if the target device has powered down or if the connection timed out. This could be caused by poor network traffic. Check the cabling, switches and connections."},
    {0x01, 0x0111, PLCTAG_ERR_UNSUPPORTED, "RPI Not Supported", "This error occurs if the Originator is specifying an RPI that is not supported. The Productivity Suite CPU will accept a minimum value of 10ms on a CIP Forward Open request. However, the CPU will produce at the specified rate up to the scan time of the installed project. The CPU cannot product any faster than the scan time of the running project."},
    {0x01, 0x0112, PLCTAG_ERR_BAD_PARAM, "RPI Value not acceptable", "This error can be returned if the Originator is specifying an RPI value that is not acceptable. There may be six additional values following the extended error code with the acceptable values. An array can be defined for this field in order to view the extended error code attributes. If the Target device supports extended status, the format of the values will be as shown below:\nUnsigned Integer 16, Value = 0x0112, Explanation: Extended Status code,\nUnsigned Integer 8, Value = variable, Explanation: Acceptable Originator to Target RPI type, values: 0 = The RPI specified in the forward open was acceptable (O -> T value is ignored), 1 = unspecified (use a different RPI), 2 = minimum acceptable RPI (too fast), 3 = maximum acceptable RPI (too slow), 4 = required RPI to corrected mismatch (data is already being consumed at a different RPI), 5 to 255 = reserved.\nUnsigned Integer 32, Value = variable, Explanation: Value of O -> T RPI that is within the acceptable range for the application.\nUnsigned Integer 32, Value = variable, Explanation: Value of T -> O RPI that is within the acceptable range for the application."},
    {0x01, 0x0113, PLCTAG_ERR_NO_RESOURCES, "Out of Connections", "The Productivity Suite EtherNet/IP Adapter connection limit of 4 when doing Class 3 connections has been reached. An existing connection must be dropped in order for a new one to be generated."},
    {0x01, 0x0114, PLCTAG_ERR_NOT_FOUND, "Vendor ID or Product Code Mismatch", "The compatibility bit was set in the Forward Open message but the Vendor ID or Product Code did not match."},
    {0x01, 0x0115, PLCTAG_ERR_NOT_FOUND, "Device Type Mismatch", "The compatibility bit was set in the Forward Open message but the Device Type did not match."},
    {0x01, 0x0116, PLCTAG_ERR_NO_MATCH, "Revision Mismatch", "The compatibility bit was set in the Forward Open message but the major and minor revision numbers were not a valid revision."},
    {0x01, 0x0117, PLCTAG_ERR_BAD_PARAM, "Invalid Produced or Consumed Application Path", "This error is returned from the Target device when the Connection Point parameters specified for the O -> T (Output) or T -> O (Input) connection is incorrect or not supported. The Productivity Suite CPU does not return this error and uses the following error codes instead: 0x012A, 0x012B or 0x012F."},
    {0x01, 0x0118, PLCTAG_ERR_BAD_PARAM, "Invalid or Inconsistent Configuration Application Path", "This error is returned from the Target device when the Connection Point parameter specified for the Configuration data is incorrect or not supported. The Productivity Suite CPU does not return this error and uses the following error codes instead: 0x0129 or 0x012F."},
    {0x01, 0x0119, PLCTAG_ERR_OPEN, "Non-listen Only Connection Not Opened", "This error code is returned when an Originator device attempts to establish a listen only connection and there is no non-listen only connection established. The Productivity Suite CPU does not support listen only connections as Scanner or Adapter."},
    {0x01, 0x011A, PLCTAG_ERR_NO_RESOURCES, "Target Object Out of Connections", "The maximum number of connections supported by this instance of the object has been exceeded."},
    {0x01, 0x011B, PLCTAG_ERR_TOO_SMALL, "RPI is smaller than the Production Inhibit Time", "The Target to Originator RPI is smaller than the Target to Originator Production Inhibit Time. Consult the manufacturer's documentation as to the minimum rate that data can be produced and adjust the RPI to greater than this value."},
    {0x01, 0x011C, PLCTAG_ERR_UNSUPPORTED, "Transport Class Not Supported", "The Transport Class requested in the Forward Open is not supported. Only Class 1 and Class 3 classes are supported in the Productivity Suite CPU."},
    {0x01, 0x011D, PLCTAG_ERR_UNSUPPORTED, "Production Trigger Not Supported", "The Production Trigger requested in the Forward Open is not supported. In Class 1, only Cyclic and Change of state are supported in the Productivity Suite CPU. In Class 3, Application object is supported."},
    {0x01, 0x011E, PLCTAG_ERR_UNSUPPORTED, "Direction Not Supported", "The Direction requested in the Forward Open is not supported."},
    {0x01, 0x011F, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Fixed/Variable Flag", "The Originator to Target fixed/variable flag specified in the Forward Open is not supported . Only Fixed is supported in the Productivity Suite CPU."},
    {0x01, 0x0120, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Fixed/Variable Flag", "The Target to Originator fixed/variable flag specified in the Forward Open is not supported. Only Fixed is supported in the Productivity Suite CPU."},
    {0x01, 0x0121, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Priority", "The Originator to Target Network Connection Priority specified in the Forward Open is not supported. Low, High, Scheduled and Urgent are supported in the Productivity Suite CPU."},
    {0x01, 0x0122, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Priority", "The Target to Originator Network Connection Priority specified in the Forward Open is not supported. Low, High, Scheduled and Urgent are supported in the Productivity Suite CPU."},
    {0x01, 0x0123, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Type", "The Originator to Target Network Connection Type specified in the Forward Open is not supported. Only Unicast is supported for O -> T (Output) data in the Productivity Suite CPU."},
    {0x01, 0x0124, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Network Connection Type", "The Target to Originator Network Connection Type specified in the Forward Open is not supported. Multicast and Unicast is supported in the Productivity Suite CPU. Some devices may not support one or the other so if this error is encountered try the other method."},
    {0x01, 0x0125, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Network Connection Redundant_Owner", "The Originator to Target Network Connection Redundant_Owner flag specified in the Forward Open is not supported. Only Exclusive owner connections are supported in the Productivity Suite CPU."},
    {0x01, 0x0126, PLCTAG_ERR_BAD_PARAM, "Invalid Configuration Size", "This error is returned when the Configuration data sent in the Forward Open does not match the size specified or is not supported by the Adapter. The Target device may return an additional Unsigned Integer 16 value that specifies the maximum size allowed for this data. An array can be defined for this field in order to view the extended error code attributes."},
    {0x01, 0x0127, PLCTAG_ERR_BAD_PARAM, "Invalid Originator to Target Size", "This error is returned when the Originator to Target (Output data) size specified in the Forward Open does not match what is in the Target. Consult the documentation of the Adapter device to verify the required size. Note that if the Run/Idle header is requested, it will add 4 additional bytes and must be accounted for in the Forward Open calculation. The Productivity Suite CPU always requires the Run/Idle header so if the option doesn't exist in the Scanner device, you must add an additional 4 bytes to the O -> T (Output) setup. Some devices may publish the size that they are looking for as an additional attribute (Unsigned Integer 16 value) of the Extended Error Code. An array can be defined for this field in order to view the extended error code attributes.\nNote: This error may also be generated when a Connection Point value that is invalid for IO Messaging (but valid for other cases such as Explicit Messaging) is specified, such as 0. Please verify if the Connection Point value is valid for IO Messaging in the target device."},
    {0x01, 0x0128, PLCTAG_ERR_BAD_PARAM, "Invalid Target to Originator Size", "This error is returned when the Target to Originator (Input data) size specified in the Forward Open does not match what is in Target. Consult the documentation of the Adapter device to verify the required size. Note that if the Run/Idle header is requested, it will add 4 additional bytes and must be accounted for in the Forward Open calculation. The Productivity Suite CPU does not support a Run/Idle header for the T -> O (Input) data. Some devices may publish the size that they are looking for as an additional attribute (Unsigned Integer 16 value) of the Extended Error Code. An array can be defined for this field in order to view the extended error code attributes.\nNote: This error may also be generated when a Connection Point value that is invalid for IO Messaging (but valid for other cases such as Explicit Messaging) is specified, such as 0. Please verify if the Connection Point value is valid for IO Messaging in the target device."},
    {0x01, 0x0129, PLCTAG_ERR_BAD_PARAM, "Invalid Configuration Application Path", "This error will be returned by the Productivity Suite CPU if a Configuration Connection with a size other than 0 is sent to the CPU. The Configuration Connection size must always be zero if it this path is present in the Forward Open message coming from the Scanner device."},
    {0x01, 0x012A, PLCTAG_ERR_BAD_PARAM, "Invalid Consuming Application Path", "This error will be returned by the Productivity Suite CPU if the Consuming (O -> T) Application Path is not present in the Forward Open message coming from the Scanner device or if the specified Connection Point is incorrect."},
    {0x01, 0x012B, PLCTAG_ERR_BAD_PARAM, "Invalid Producing Application Path", "This error will be returned by the Productivity Suite CPU if the Producing (T -> O) Application Path is not present in the Forward Open message coming from the Scanner device or if the specified Connection Point is incorrect."},
    {0x01, 0x012C, PLCTAG_ERR_NOT_FOUND, "Configuration Symbol Does not Exist", "The Originator attempted to connect to a configuration tag name that is not supported in the Target."},
    {0x01, 0x012D, PLCTAG_ERR_NOT_FOUND, "Consuming Symbol Does not Exist", "The Originator attempted to connect to a consuming tag name that is not supported in the Target."},
    {0x01, 0x012E, PLCTAG_ERR_NOT_FOUND, "Producing Symbol Does not Exist", "The Originator attempted to connect to a producing tag name that is not supported in the Target."},
    {0x01, 0x012F, PLCTAG_ERR_BAD_DATA, "Inconsistent Application Path Combination", "The combination of Configuration, Consuming and Producing application paths specified are inconsistent."},
    {0x01, 0x0130, PLCTAG_ERR_BAD_DATA, "Inconsistent Consume data format", "Information in the data segment not consistent with the format of the data in the consumed data."},
    {0x01, 0x0131, PLCTAG_ERR_BAD_DATA, "Inconsistent Product data format", "Information in the data segment not consistent with the format of the data in the produced data."},
    {0x01, 0x0132, PLCTAG_ERR_UNSUPPORTED, "Null Forward Open function not supported", "The target device does not support the function requested in the NULL Forward Open request. The request could be such items as Ping device, Configure device application, etc."},
    {0x01, 0x0133, PLCTAG_ERR_BAD_PARAM, "Connection Timeout Multiplier not acceptable", "The Connection Multiplier specified in the Forward Open request not acceptable by the Target device (once multiplied in conjunction with the specified timeout value). Consult the manufacturer device's documentation on what the acceptable timeout and multiplier are for this device."},
    {0x01, 0x0203, PLCTAG_ERR_TIMEOUT, "Connection Timed Out", "This error will be returned by the Productivity Suite CPU if a message is sent to the CPU on a connection that has already timed out. Connections time out if no message is sent to the CPU in the time period specified by the RPI rate X Connection multiplier specified in the Forward Open message."},
    {0x01, 0x0204, PLCTAG_ERR_TIMEOUT, "Unconnected Request Timed Out", "This time out occurs when the device sends an Unconnected Request and no response is received within the specified time out period. In the Productivity Suite CPU, this value may be found in the hardware configuration under the Ethernet port settings for the P3-550 or P3-530."},
    {0x01, 0x0205, PLCTAG_ERR_BAD_PARAM, "Parameter Error in Unconnected Request Service", "This error occurs when Connection Tick Time/Connection time-out combination is specified in the Forward Open or Forward Close message is not supported by the device."},
    {0x01, 0x0206, PLCTAG_ERR_TOO_LARGE, "Message Too Large for Unconnected_Send Service", "Occurs when Unconnected_Send message is too large to be sent to the network."},
    {0x01, 0x0207, PLCTAG_ERR_BAD_REPLY, "Unconnected Acknowledge without Reply", "This error occurs if an Acknowledge was received but no data response occurred. Verify that the message that was sent is supported by the Target device using the device manufacturer's documentation."},
    {0x01, 0x0301, PLCTAG_ERR_NO_MEM, "No Buffer Memory Available", "This error occurs if the Connection memory buffer in the target device is full. Correct this by reducing the frequency of the messages being sent to the device and/or reducing the number of connections to the device. Consult the manufacturer's documentation for other means of correcting this."},
    {0x01, 0x0302, PLCTAG_ERR_NO_RESOURCES, "Network Bandwidth not Available for Data", "This error occurs if the Producer device cannot support the specified RPI rate when the connection has been configured with schedule priority. Reduce the RPI rate or consult the manufacturer's documentation for other means to correct this."},
    {0x01, 0x0303, PLCTAG_ERR_NO_RESOURCES, "No Consumed Connection ID Filter Available", "This error occurs if a Consumer device doesn't have an available consumed_connection_id filter."},
    {0x01, 0x0304, PLCTAG_ERR_BAD_CONFIG, "Not Configured to Send Scheduled Priority Data", "This error occurs if a device has been configured for a scheduled priority message and it cannot send the data at the scheduled time slot."},
    {0x01, 0x0305, PLCTAG_ERR_NO_MATCH, "Schedule Signature Mismatch", "This error occurs if the schedule priority information does not match between the Target and the Originator."},
    {0x01, 0x0306, PLCTAG_ERR_UNSUPPORTED, "Schedule Signature Validation not Possible", "This error occurs when the schedule priority information sent to the device is not validated."},
    {0x01, 0x0311, PLCTAG_ERR_BAD_DEVICE, "Port Not Available", "This error occurs when a port number specified in a port segment is not available. Consult the documentation of the device to verify the correct port number."},
    {0x01, 0x0312, PLCTAG_ERR_BAD_PARAM, "Link Address Not Valid", "The Link address specified in the port segment is not correct. Consult the documentation of the device to verify the correct port number."},
    {0x01, 0x0315, PLCTAG_ERR_BAD_PARAM, "Invalid Segment in Connection Path", "This error occurs when the target device cannot understand the segment type or segment value in the Connection Path. Consult the documentation of the device to verify the correct segment type and value. If a Connection Point greater than 255 is specified this error could occur."},
    {0x01, 0x0316, PLCTAG_ERR_NO_MATCH, "Forward Close Service Connection Path Mismatch", "This error occurs when the Connection path in the Forward Close message does not match the Connection Path configured in the connection. Contact Tech Support if this error persists."},
    {0x01, 0x0317, PLCTAG_ERR_BAD_PARAM, "Scheduling Not Specified", "This error can occur if the Schedule network segment or value is invalid."},
    {0x01, 0x0318, PLCTAG_ERR_BAD_PARAM, "Link Address to Self Invalid", "If the Link address points back to the originator device, this error will occur."},
    {0x01, 0x0319, PLCTAG_ERR_NO_RESOURCES, "Secondary Resource Unavailable", "This occurs in a redundant system when the secondary connection request is unable to duplicate the primary connection request."},
    {0x01, 0x031A, PLCTAG_ERR_DUPLICATE, "Rack Connection Already established", "The connection to a module is refused because part or all of the data requested is already part of an existing rack connection."},
    {0x01, 0x031B, PLCTAG_ERR_DUPLICATE, "Module Connection Already established", "The connection to a rack is refused because part or all of the data requested is already part of an existing module connection."},
    {0x01, 0x031C, PLCTAG_ERR_REMOTE_ERR, "Miscellaneous", "This error is returned when there is no other applicable code for the error condition. Consult the manufacturer's documentation or contact Tech support if this error persist."},
    {0x01, 0x031D, PLCTAG_ERR_NO_MATCH, "Redundant Connection Mismatch", "This error occurs when these parameters don't match when establishing a redundant owner connection: O -> T RPI, O -> T Connection Parameters, T -> O RPI, T -> O Connection Parameters and Transport Type and Trigger."},
    {0x01, 0x031E, PLCTAG_ERR_NO_RESOURCES, "No more User Configurable Link Resources Available in the Producing Module", "This error is returned from the Target device when no more available Consumer connections available for a Producer."},
    {0x01, 0x031F, PLCTAG_ERR_NO_RESOURCES, "No User Configurable Link Consumer Resources Configured in the Producing Module", "This error is returned from the Target device when no Consumer connections have been configured for a Producer connection."},
    {0x01, 0x0800, PLCTAG_ERR_BAD_DEVICE, "Network Link Offline", "The Link path is invalid or not available."},
    {0x01, 0x0810, PLCTAG_ERR_NO_DATA, "No Target Application Data Available", "This error is returned from the Target device when the application has no valid data to produce."},
    {0x01, 0x0811, PLCTAG_ERR_NO_DATA, "No Originator Application Data Available", "This error is returned from the Originator device when the application has no valid data to produce."},
    {0x01, 0x0812, PLCTAG_ERR_UNSUPPORTED, "Node Address has changed since the Network was scheduled", "This specifies that the router has changed node addresses since the value configured in the original connection."},
    {0x01, 0x0813, PLCTAG_ERR_UNSUPPORTED, "Not Configured for Off-subnet Multicast", "The producer has been requested to support a Multicast connection for a consumer on a different subnet and does not support this functionality."},
    {0x01, 0x0814, PLCTAG_ERR_BAD_DATA, "Invalid Produce/Consume Data format", "Information in the data segment not consistent with the format of the data in the consumed or produced data. Errors 0x0130 and 0x0131 are typically used for this situation in most devices now."},
    {0x02, -1, PLCTAG_ERR_NO_RESOURCES, "Resource Unavailable for Unconnected Send", "The Target device does not have the resources to process the Unconnected Send request."},
    {0x03, -1, PLCTAG_ERR_BAD_PARAM, "Parameter value invalid.", ""},
    {0x04, -1, PLCTAG_ERR_BAD_DATA,"IOI could not be deciphered or tag does not exist.", "The path segment identifier or the segment syntax was not understood by the target device."},
    {0x05, -1, PLCTAG_ERR_BAD_PARAM, "Path Destination Error", "The Class, Instance or Attribute value specified in the Unconnected Explicit Message request is incorrect or not supported in the Target device. Check the manufacturer's documentation for the correct codes to use."},
    {0x06, -1, PLCTAG_ERR_TOO_LARGE, "Data requested would not fit in response packet.", "The data to be read/written needs to be broken up into multiple packets.0x070000 Connection lost: The messaging connection was lost."},
    {0x07, -1, PLCTAG_ERR_BAD_CONNECTION, "Connection lost", "The messaging connection was lost."},
    {0x08, -1, PLCTAG_ERR_UNSUPPORTED, "Unsupported service.", ""},
    {0x09, -1, PLCTAG_ERR_BAD_DATA, "Error in Data Segment", "This error code is returned when an error is encountered in the Data segment portion of a Forward Open message. The Extended Status value is the offset in the Data segment where the error was encountered."},
    {0x0A, -1, PLCTAG_ERR_BAD_STATUS, "Attribute list error", "An attribute in the Get_Attribute_List or Set_Attribute_List response has a non-zero status."},
    {0x0B, -1, PLCTAG_ERR_DUPLICATE, "Already in requested mode/state", "The object is already in the mode/state being requested by the service."},
    {0x0C, -1, PLCTAG_ERR_BAD_STATUS, "Object State Error", "This error is returned from the Target device when the current state of the Object requested does not allow it to be returned. The current state can be specified in the Optional Extended Error status field."},
    {0x0D, -1, PLCTAG_ERR_DUPLICATE, "Object already exists.", "The requested instance of object to be created already exists."},
    {0x0E, -1, PLCTAG_ERR_NOT_ALLOWED, "Attribute not settable", "A request to modify non-modifiable attribute was received."},
    {0x0F, -1, PLCTAG_ERR_NOT_ALLOWED, "Permission denied.", ""},
    {0x10, -1, PLCTAG_ERR_BAD_STATUS, "Device State Error", "This error is returned from the Target device when the current state of the Device requested does not allow it to be returned. The current state can be specified in the Optional Extended Error status field. Check your configured connections points for other Client devices using this same connection."},
    {0x11, -1, PLCTAG_ERR_TOO_LARGE, "Reply data too large", "The data to be transmitted in the response buffer is larger than the allocated response buffer."},
    {0x12, -1, PLCTAG_ERR_NOT_ALLOWED, "Fragmentation of a primitive value", "The service specified an operation that is going to fragment a primitive data value. For example, trying to send a 2 byte value to a REAL data type (4 byte)."},
    {0x13, -1, PLCTAG_ERR_TOO_SMALL, "Not Enough Data", "Not enough data was supplied in the service request specified."},
    {0x14, -1, PLCTAG_ERR_UNSUPPORTED, "Attribute not supported.", "The attribute specified in the request is not supported."},
    {0x15, -1, PLCTAG_ERR_TOO_LARGE, "Too Much Data", "Too much data was supplied in the service request specified."},
    {0x16, -1, PLCTAG_ERR_NOT_FOUND, "Object does not exist.", "The object specified does not exist in the device."},
    {0x17, -1, PLCTAG_ERR_NOT_ALLOWED, "Service fragmentation sequence not in progress.", "The fragmentation sequence for this service is not currently active for this data."},
    {0x18, -1, PLCTAG_ERR_NO_DATA, "No stored attribute data.", "The attribute data of this object was not saved prior to the requested service."},
    {0x19, -1, PLCTAG_ERR_REMOTE_ERR, "Store operation failure.", "The attribute data of this object was not saved due to a failure during the attempt."},
    {0x1A, -1, PLCTAG_ERR_TOO_LARGE, "Routing failure, request packet too large.", "The service request packet was too large for transmission on a network in the path to the destination."},
    {0x1B, -1, PLCTAG_ERR_TOO_LARGE, "Routing failure, response packet too large.", "The service reponse packet was too large for transmission on a network in the path from the destination."},
    {0x1C, -1, PLCTAG_ERR_NO_DATA, "Missing attribute list entry data.", "The service did not supply an attribute in a list of attributes that was needed by the service to perform the requested behavior."},
    {0x1D, -1, PLCTAG_ERR_BAD_DATA, "Invalid attribute value list.", "The service is returning the list of attributes supplied with status information for those attributes that were invalid."},
    {0x20, -1, PLCTAG_ERR_BAD_PARAM, "Invalid parameter.", "A parameter associated with the request was invalid. This code is used when a parameter does meet the requirements defined in an Application Object specification."},
    {0x21, -1, PLCTAG_ERR_DUPLICATE, "Write-once value or medium already written.", "An attempt was made to write to a write-once-medium that has already been written or to modify a value that cannot be change once established."},
    {0x22, -1, PLCTAG_ERR_BAD_REPLY, "Invalid Reply Received", "An invalid reply is received (example: service code sent doesn't match service code received.)."},
    {0x25, -1, PLCTAG_ERR_BAD_PARAM, "Key failure in path", "The key segment was included as the first segment in the path does not match the destination module."},
    {0x26, -1, PLCTAG_ERR_BAD_PARAM, "The number of IOI words specified does not match IOI word count.", "Check the tag length against what was sent."},
    {0x27, -1, PLCTAG_ERR_BAD_PARAM, "Unexpected attribute in list", "An attempt was made to set an attribute that is not able to be set at this time."},
    {0x28, -1, PLCTAG_ERR_BAD_PARAM, "Invalid Member ID.", "The Member ID specified in the request does not exist in the specified Class/Instance/Attribute."},
    {0x29, -1, PLCTAG_ERR_NOT_ALLOWED, "Member not writable.", "A request to modify a non-modifiable member was received."},
    {0xFF, 0x2104, PLCTAG_ERR_OUT_OF_BOUNDS, "Address is out of range.",""},
    {0xFF, 0x2105, PLCTAG_ERR_OUT_OF_BOUNDS, "Attempt to access beyond the end of the data object.", ""},
    {0xFF, 0x2107, PLCTAG_ERR_BAD_PARAM, "The data type is invalid or not supported.", ""},
    {-1, -1, PLCTAG_ERR_REMOTE_ERR, "Unknown error code.", "Unknown error code."}
};





static int lookup_error_code(uint8_t *data)
{
    int index = 0;
    int primary_code = 0;
    int secondary_code = 0;

    /* build the error status */
    primary_code = (int)*data;

    if(primary_code != 0) {
        int num_status_words = 0;

        data++;
        num_status_words = (int)*data;

        if(num_status_words > 0) {
            data++;
            secondary_code = (int)data[0] + (int)(data[1] << 8);
        }
    }

    while(error_code_table[index].primary_code != -1) {
        if(error_code_table[index].primary_code == primary_code) {
            if(error_code_table[index].secondary_code == secondary_code || error_code_table[index].secondary_code == -1) {
                break;
            }
        }

        index++;
    }

    return index;
}




const char *decode_cip_error_short(uint8_t *data)
{
    int index = lookup_error_code(data);

    return error_code_table[index].short_desc;
}


const char *decode_cip_error_long(uint8_t *data)
{
    int index = lookup_error_code(data);

    return error_code_table[index].long_desc;
}


int decode_cip_error_code(uint8_t *data)
{
    int index = lookup_error_code(data);

    return error_code_table[index].translated_code;
}
