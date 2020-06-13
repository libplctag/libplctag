/***************************************************************************
 *   Copyright (C) 2020 by Kyle Hayes                                      *
 *   Author Kyle Hayes  kyle.hayes@gmail.com                               *
 *                                                                         *
 * This software is available under either the Mozilla Public License      *
 * version 2.0 or the GNU LGPL version 2 (or later) license, whichever     *
 * you choose.                                                             *
 *                                                                         *
 * MPL 2.0:                                                                *
 *                                                                         *
 *   This Source Code Form is subject to the terms of the Mozilla Public   *
 *   License, v. 2.0. If a copy of the MPL was not distributed with this   *
 *   file, You can obtain one at http://mozilla.org/MPL/2.0/.              *
 *                                                                         *
 *                                                                         *
 * LGPL 2:                                                                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

import libplctag.Tag;

public class TagRW {
	enum DataType { UNKNOWN, SINT8, UINT8, SINT16, UINT16, SINT32, UINT32, SINT64, REAL32, REAL64 };

	
	private static final int CREATE_TIMEOUT = 2000; // two seconds
	private static final int OP_TIMEOUT = 2000; // two seconds
	
	private static DataType dType = DataType.UNKNOWN;
	private static int dSize = 0;
	private static String attribs;
	private static boolean hasWrite = false;
	private static String writeStr;
	
    public static void main (String[] args) {
    	int rc = Tag.PLCTAG_STATUS_OK;
    	
        parseArgs(args);
        
        Tag tag = new Tag(attribs, CREATE_TIMEOUT);
        
        if(tag.status() != Tag.PLCTAG_STATUS_OK) {
        	System.err.println("Tag creation failed with error: " + tag.decodeError(tag.status()));
        	tag.close();
        	System.exit(1);
        }
        
        if(hasWrite) {
        	doWrite(tag);
        	System.out.println("Wrote tag value successfully.");
        }
        
        doRead(tag);
        System.out.println("Read tag value successfully.");
    }
    
    
    private static void doWrite(Tag tag) {
    	int rc = Tag.PLCTAG_STATUS_OK;
    	int iVal = 0;
    	long lVal = 0;
    	double fVal = 0.0;
    	
    	switch(dType) {
    	case SINT8:
    		iVal = Integer.parseInt(writeStr);
    		rc  = tag.setInt8(0, iVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
    		break;
		case UINT8:
    		iVal = Integer.parseInt(writeStr);
    		rc  = tag.setUInt8(0, iVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
			break;

    	case SINT16:
    		iVal = Integer.parseInt(writeStr);
    		rc  = tag.setInt16(0, iVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
    		break;
		case UINT16:
    		iVal = Integer.parseInt(writeStr);
    		rc  = tag.setUInt16(0, iVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
			break;

    	case SINT32:
    		iVal = Integer.parseInt(writeStr);
    		rc  = tag.setInt32(0, iVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
    		break;
		case UINT32:
    		iVal = Integer.parseInt(writeStr);
    		rc  = tag.setUInt32(0, iVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
			break;

    	case SINT64:
    		lVal = Long.parseLong(writeStr);
    		rc  = tag.setInt64(0, lVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
    		break;

    	case REAL32:
    		fVal = Double.parseDouble(writeStr);
    		rc  = tag.setFloat32(0, (float) fVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
			break;

		case REAL64:
    		fVal = Double.parseDouble(writeStr);
    		rc  = tag.setFloat64(0, fVal);
    		if(rc != Tag.PLCTAG_STATUS_OK) {
            	System.err.println("Setting tag value failed with error: " + tag.decodeError(rc));
            	tag.close();
            	System.exit(1);    			
    		}
			break;

		default:
			System.err.println("Unsupported data type!");
			System.exit(1);
			break;
    	}
    	
    	rc = tag.write(OP_TIMEOUT);
		if(rc != Tag.PLCTAG_STATUS_OK) {
        	System.err.println("Writing tag failed with error: " + tag.decodeError(rc));
        	tag.close();
        	System.exit(1);    			
		}
    }
    
    
    private static void doRead(Tag tag) {
    	int rc = Tag.PLCTAG_STATUS_OK;
    	int iVal = 0;
    	long lVal = 0;
    	double fVal = 0.0;
    	int index = 0;
    	int numElems = 0;
    	
    	rc = tag.read(OP_TIMEOUT);
		if(rc != Tag.PLCTAG_STATUS_OK) {
        	System.err.println("Reading tag failed with error: " + tag.decodeError(rc));
        	tag.close();
        	System.exit(1);    			
		}

		numElems = tag.size() / dSize;
		
		for(index = 0; index < numElems; index++) {
			switch(dType) {
			case UINT8:
				iVal = tag.getUInt8(index * dSize);				
				System.out.println("data["  + index + "] = " + iVal);
				break;

			case SINT8:
				iVal = tag.getInt8(index * dSize);				
				System.out.println("data["  + index + "] = " + iVal);
				break;
				
			case UINT16:
				iVal = tag.getUInt16(index * dSize);				
				System.out.println("data["  + index + "] = " + iVal);
				break;
				
			case SINT16:
				iVal = tag.getInt16(index * dSize);				
				System.out.println("data["  + index + "] = " + iVal);
				break;
				
			case UINT32:
				lVal = tag.getUInt32(index * dSize);				
				System.out.println("data["  + index + "] = " + lVal);
				break;
				
			case SINT32:
				iVal = tag.getInt32(index * dSize);				
				System.out.println("data["  + index + "] = " + iVal);
				break;
				
			case SINT64:
				lVal = tag.getInt64(index * dSize);				
				System.out.println("data["  + index + "] = " + lVal);
				break;
				
			case REAL32:
				fVal = tag.getFloat32(index * dSize);				
				System.out.println("data["  + index + "] = " + fVal);
				break;
				
			case REAL64:
				fVal = tag.getFloat64(index * dSize);
				System.out.println("data["  + index + "] = " + fVal);
				break;

				
			default:
				System.err.println("Unsupported data type!");
				System.exit(1);
				break;
			}
		}
	}
    
    private static void parseArgs(String[] args) {
    	int i = 0;
    	
    	for(i=0; i<args.length; i++) {
    		if(args[i].equalsIgnoreCase("-t")) {
    			if((i+1) >= args.length) {
    				System.err.println("-t param requires a type!");
    				System.exit(1);
    			}
    			
    			parseTypeArg(args[i+1]);
    			i++;
    		} else if("-p".equalsIgnoreCase(args[i])) {
    			if((i+1) >= args.length) {
    				System.err.println("-p param requires a value!");
    				System.exit(1);
    			}
    			
    			parseAttribArg(args[i+1]);
    			i++;
    		} else if("-w".equalsIgnoreCase(args[i])) {
    			if((i+1) >= args.length) {
    				System.err.println("-w param requires a value!");
    				System.exit(1);
    			}
    			
    			hasWrite = true;
    			parseWriteArg(args[i+1]);
    			i++;
    		} else {
				System.err.println("Unknown argument: " + args[i]);
				System.exit(1);    			
    		}
    	}
    }
    
    
    private static void parseTypeArg(String typeArg) {
    	if("uint8".equalsIgnoreCase(typeArg)) {
    		dType = DataType.UINT8;
    		dSize = 1;
    	} else if("sint8".equalsIgnoreCase(typeArg)) {
    		dType = DataType.SINT8;
    		dSize = 1;
    	} else if("uint16".equalsIgnoreCase(typeArg)) {
    		dType = DataType.UINT16;
    		dSize = 2;
    	} else if("sint16".equalsIgnoreCase(typeArg)) {
    		dType = DataType.SINT16;
    		dSize = 2;
    	} else if("uint32".equalsIgnoreCase(typeArg)) {
    		dType = DataType.UINT32;
    		dSize = 4;
    	} else if("sint32".equalsIgnoreCase(typeArg)) {
    		dType = DataType.SINT32;
    		dSize = 4;
    	} else if("sint64".equalsIgnoreCase(typeArg)) {
    		dType = DataType.SINT64;
    		dSize = 8;
    	} else if("float32".equalsIgnoreCase(typeArg)) {
    		dType = DataType.REAL32;
    		dSize = 4;
    	} else if("float64".equalsIgnoreCase(typeArg)) {
    		dType = DataType.REAL64;
    		dSize = 8;
    	} else {
			System.err.println("Unknown type: " + typeArg);
			System.exit(1);
    	}
    }
    
    
    private static void parseAttribArg(String attribArg) {
    	if(attribArg.isEmpty()) {
			System.err.println("Attribute string must not be empty or missing!");
			System.exit(1);
    	}
    	
    	attribs = attribArg;
    }

    
    private static void parseWriteArg(String writeArg) {
    	writeStr = writeArg;
    }
}
