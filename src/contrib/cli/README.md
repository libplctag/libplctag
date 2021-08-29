# LIBPLCTAG Command-Line Interface

This is a command line program extension to the _libplctag_. There are three basic types of operation supported by the program:

- __READ__: read the value of the tag specified once and return the value read.
- __WRITE__: write the value provided to the tag specified once and return the resulting value of the tag.
- __WATCH__: read the value of the specified tag on start and return it. Then perform subsequent periodic reads for the interval specified and only return the value if it has changed since the last known value.

## Pre-requisites
The CLI is pre-compiled with all the library source code, so there are typically no required dependencies on Linux/MacOS.

On Windows make sure that the Visual C Runtime is present on the system path. The specific file required is _vcruntime140.dll_.

## Usage
To use this CLI you need to specify and run a certain operation on each execution, with the specific _libplctag_ parameters for your specific PLC. Below is the description for the available command-line options. 

```
cli {--read | --write | --watch} {-protocol} {-ip} {-path} {-plc}
                [-debug] [-interval] [-attributes] [-offline]

        CLI Action (Required):

        --read          - Perform a one-shot READ operation.
        --write         - Perform a one-shot WRITE operation.
        --watch         - Perform a continuous WATCH operation at the specified interval.
        -h | --help     - Prints the Usage details.

        LIBPLCTAG Parameters (Required):

        -protocol       - type of plc protocol. (default: ab_eip)
        -ip             - network address for the host PLC. (default: 127.0.0.1)
        -path           - routing path for the Tags. (default: 1,0)
        -plc            - type of the PLC. (default: controllogix)

        LIBPLCTAG Parameters (Optional):

        -debug          - logging output level. (default: 1)
        -interval       - interval in ms for WATCH operation. (default: 500)
        -attributes     - additional attributes. (default: '')
        -offline        - operation mode. (default: false)
```

The program then waits for the inputs through the _stdin_, specify the tags per line and pass in a _'break'_ keyword to start the operation on the passed tags. Alternatively, a file with the tag inputs per line works too, in case of several tags to avoid manual entry each time. 

Below is the format the CLI expects input, and return the output in.

## Inputs
The following format is expected per line by the CLI when processing tags before kicking off the operation requested.

```
key=TestTag,type=uint16,bit=8,offset=3,path=PLCTestVar.SampleValue
```
NOTE that _key_, _type_ and _path_ parameter are required, while the rest are optional for bit and offset memory variable access. For the __WRITE__ operation an additional _value_ parameter is required. 

## Outputs
The following is the format the CLI returns the outputs for the requested operation.

```
{"key":value}
```
