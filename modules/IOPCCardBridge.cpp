/*======================================================================

    The contents of this file are subject to the Mozilla Public
    License Version 1.1 (the "License"); you may not use this file
    except in compliance with the License. You may obtain a copy of
    the License at http://www.mozilla.org/MPL/

    Software distributed under the License is distributed on an "AS
    IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
    implied. See the License for the specific language governing
    rights and limitations under the License.

    The initial developer of the original code is David A. Hinds
    <dahinds@users.sourceforge.net>.  Portions created by David A. Hinds
    are Copyright (C) 1999 David A. Hinds.  All Rights Reserved.

    Contributor:  Apple Computer, Inc.  Portions � 2000 Apple Computer, 
    Inc. All rights reserved.

    Alternatively, the contents of this file may be used under the
    terms of the GNU Public License version 2 (the "GPL"), in which
    case the provisions of the GPL are applicable instead of the
    above.  If you wish to allow the use of your version of this file
    only under the terms of the GPL and not to allow others to use
    your version of this file under the MPL, indicate your decision
    by deleting the provisions above and replace them with the notice
    and other provisions required by the GPL.  If you do not delete
    the provisions above, a recipient may use your version of this
    file under either the MPL or the GPL.

    Portions of this code are derived from:

	ds.c 1.108 2000/08/07 19:06:15
    
======================================================================*/


#define IN_CARD_SERVICES
#include <IOKit/pccard/IOPCCard.h>

__BEGIN_DECLS
#include <IOKit/pccard/ss.h>
#include "yenta.h"
__END_DECLS

#ifdef PCMCIA_DEBUG
int ds_debug = PCMCIA_DEBUG;
MODULE_PARM(ds_debug, "i");
#define DEBUG(n, args...) if (ds_debug>(n)) printk(KERN_DEBUG args)
static const char *version =
"ds.c 1.104 2000/01/11 01:18:02 (David Hinds)";
#else
#define DEBUG(n, args...)
#endif

/* Socket state information */
typedef struct socket_info_t {
    IOPCCardBridge *	bridge;
    client_handle_t	handle;
    config_req_t	config;
    client_handle_t	configHandle;
    int			state;
    struct timer_list	removal;
    OSArray *		nubs;
} socket_info_t;

#define SOCKET_PRESENT		0x01
#define SOCKET_BUSY		0x02
#define SOCKET_CONFIG		0x04
#define SOCKET_REMOVAL_PENDING	0x08
#define SOCKET_SUSPEND		0x10

static unsigned int sockets = 0;
static socket_info_t *socket_table = NULL;

/* Device driver ID passed to Card Services */
static dev_info_t dev_info = "Driver Services";

#define kInterruptControllerNamePrefix "IOPCCardInterruptController"

//*****************************************************************************
//				global variables
//*****************************************************************************

static IOLock * 	gIOPCCardBridgeLock;

static int    		gIOPCCardConfigurationInited = 0;
static int    		gIOPCCardGlobalsInited = 0;
static int		gIOPCCardResourcesInited = 0;

// configuration variables
static OSArray *	gAvailableIORanges = 0;
static OSArray *	gAvailableMemRanges = 0;
//static OSArray *	gAvailableIRQRanges = 0;

// "globals" variables
static IOWorkLoop *	gIOPCCardWorkLoop = 0;
IOCommandGate *		gIOPCCardCommandGate = 0;
OSSet *	 		gIOPCCardMappedBridgeSpace = 0;

// resource variables
static IODeviceMemory * gDynamicBridgeSpace;
static IODeviceMemory *	gDynamicBridgeIOSpace;

//*****************************************************************************
//				helper functions
//*****************************************************************************

static inline void
cs_error(client_handle_t handle, int func, int ret)
{
    error_info_t err = { func, ret };
    CardServices(ReportError, handle, &err, NULL);
}

static int
read_tuple(client_handle_t handle, cisdata_t code, void *parse, u_int attributes = 0)
{
    tuple_t tuple;
    static cisdata_t buf[255];
    int ret;
    
    tuple.DesiredTuple = code;
    tuple.Attributes = attributes;
    ret = CardServices(GetFirstTuple, handle, &tuple, NULL);
    if (ret != CS_SUCCESS) return ret;
    tuple.TupleData = buf;
    tuple.TupleOffset = 0;
    tuple.TupleDataMax = sizeof(buf);
    ret = CardServices(GetTupleData, handle, &tuple, NULL);
    if (ret != CS_SUCCESS) return ret;
    ret = CardServices(ParseTuple, handle, &tuple, parse);

    return ret;
}

bool static
wackBridgeBusNumbers(IOPCIDevice * bridgeDevice)
{
    UInt32	value = bridgeDevice->configRead32(CB_PRIMARY_BUS);
    DEBUG(2, "wackBridgeBusNumbers sub/cardbus/pci before 0x%x\n", value & 0xffffff);
#ifdef PCMCIA_DEBUG
    // debug card 
    if ((value & 0x00ffff00) == 0x000400) {
	static int nextBus = 1;
		
	value &= 0xff0000ff;
	value |= (nextBus << 8) | ((nextBus+3) << 16);
	bridgeDevice->configWrite32(CB_PRIMARY_BUS, value);

	nextBus += 5;
    }
#endif
    if (!(value & 0x00ffffff)) {
	static int nextBus = 1;

	value |= (nextBus << 8) | ((nextBus+3) << 16);
	bridgeDevice->configWrite32(CB_PRIMARY_BUS, value);
		
	nextBus += 5;
    }
#ifdef PCMCIA_DEBUG
    value = bridgeDevice->configRead32(CB_PRIMARY_BUS);
#endif
    DEBUG(2, "wackBridgeBusNumbers sub/cardbus/pci after  0x%x\n", value & 0xffffff);

    return true;
}

// Determine if the cardbus card is a PCI to PCI bridge.
// And if it is then set this state variable to bypass all card services. ;-)

static bool
hasPCIExpansionChassis(IOService * provider)
{
    IORegistryEntry * entry;
    OSIterator * iter;
    bool foundIt = false;

    if (iter = provider->getChildIterator(gIODTPlane)) {
	while (entry = OSDynamicCast(IORegistryEntry, iter->getNextObject())) {
	    const char *name = entry->getName();
	    if (name && (strcmp ("pci-bridge", name) == 0)) {
		foundIt = true;
		break;
	    }
	}
	iter->release();
    }
    return foundIt;
}

//*****************************************************************************
//*****************************************************************************

#undef  super
#define super IOPCI2PCIBridge

OSDefineMetaClassAndStructorsWithInit(IOPCCardBridge, IOPCI2PCIBridge, IOPCCardBridge::metaInit());

OSMetaClassDefineReservedUsed(IOPCCardBridge,  0);		// requestCardEjection
OSMetaClassDefineReservedUnused(IOPCCardBridge,  1);
OSMetaClassDefineReservedUnused(IOPCCardBridge,  2);
OSMetaClassDefineReservedUnused(IOPCCardBridge,  3);
OSMetaClassDefineReservedUnused(IOPCCardBridge,  4);
OSMetaClassDefineReservedUnused(IOPCCardBridge,  5);
OSMetaClassDefineReservedUnused(IOPCCardBridge,  6);
OSMetaClassDefineReservedUnused(IOPCCardBridge,  7);
OSMetaClassDefineReservedUnused(IOPCCardBridge,  8);
OSMetaClassDefineReservedUnused(IOPCCardBridge,  9);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 10);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 11);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 12);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 13);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 14);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 15);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 16);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 17);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 18);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 19);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 20);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 21);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 22);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 23);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 24);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 25);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 26);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 27);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 28);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 29);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 30);
OSMetaClassDefineReservedUnused(IOPCCardBridge, 31);

void
IOPCCardBridge::metaInit(void)
{
#ifdef PCMCIA_DEBUG
    extern boolean_t zone_check;
    zone_check = TRUE;
#endif
    gIOPCCardBridgeLock = IOLockAlloc();

    init_pcmcia_cs();
}

void
IOPCCardBridge::free(void)
{
    DEBUG(2, "IOPCCardBridge::free\n");
    
    IOLockLock(gIOPCCardBridgeLock);
    
    if (gIOPCCardConfigurationInited > 1) {
	gIOPCCardConfigurationInited--;
    } else if (gIOPCCardConfigurationInited == 1) {
	gAvailableIORanges->release();
	gAvailableMemRanges->release();
	gIOPCCardConfigurationInited = 0;
    }

    if (gIOPCCardGlobalsInited > 1) {
	gIOPCCardGlobalsInited--;
    } else if (gIOPCCardGlobalsInited == 1) {
	gIOPCCardMappedBridgeSpace->release();
	gIOPCCardWorkLoop->removeEventSource(gIOPCCardCommandGate);
	gIOPCCardCommandGate->release();
	gIOPCCardWorkLoop->release();
	gIOPCCardGlobalsInited = 0;
    }
    
    if (gIOPCCardResourcesInited > 1) {
	gIOPCCardResourcesInited--;
    } else if (gIOPCCardResourcesInited == 1) {
	gDynamicBridgeSpace->release();
	gDynamicBridgeIOSpace->release();
	gIOPCCardResourcesInited = 0;
    }
    
    IOLockUnlock(gIOPCCardBridgeLock); 
    
    // gIOPCCardBridgeLock is not freed

    super::free();
}

UInt8
IOPCCardBridge::firstBusNum(void)
{
    return bridgeDevice->configRead8(0x19);
}

UInt8 
IOPCCardBridge::lastBusNum(void)
{
    return bridgeDevice->configRead8(0x1a);
}

void
IOPCCardBridge::probeBus(IOService * provider, UInt8 busNum)
{
    // if there is a pci expansion chassis attached, just act like a pci to pci bridge
    if (pciExpansionChassis) {
    	super::probeBus(provider, busNum);
    }

    // if not, it really doesn't make sense to probe for PC Cards at this point,
    // just wait for a card insertion event
    return;
}

IOReturn
IOPCCardBridge::getNubResources(IOService * service)
{
    IOReturn	err;
    
    if (pciExpansionChassis) {
	err = super::getNubResources(service);
    } else {
	IOPCIDevice * nub = (IOPCIDevice *) service;

	if (service->getDeviceMemory())
	    return kIOReturnSuccess;

	err = getNubAddressing(nub);
    }

    return err;
}

//*****************************************************************************

bool
IOPCCardBridge::getModuleParameters(void)
{
#ifdef PCMCIA_DEBUG
    OSDictionary * debugDict = OSDynamicCast(OSDictionary, getProperty("Debug Settings"));
    if (debugDict) {
	extern int pc_debug, cb_debug, i82365_debug;
	OSNumber *	debugProp;

	debugProp = OSDynamicCast(OSNumber, debugDict->getObject("Driver Services"));
	if (debugProp) ds_debug = debugProp->unsigned32BitValue();

	debugProp = OSDynamicCast(OSNumber, debugDict->getObject("Card Services"));
	if (debugProp) pc_debug = debugProp->unsigned32BitValue();

	debugProp = OSDynamicCast(OSNumber, debugDict->getObject("Card Bus"));
	if (debugProp) cb_debug = debugProp->unsigned32BitValue();

	debugProp = OSDynamicCast(OSNumber, debugDict->getObject("i82365"));
	if (debugProp) i82365_debug = debugProp->unsigned32BitValue();
    }
#endif

    return true;
}

bool
IOPCCardBridge::getOFConfigurationSettings(OSArray **ioRanges, OSArray **memRanges)
{
    // find all cardbus controllers in the system, and build a list of all
    // the i/o and memory address ranges that have been configured.

    OSIterator * iter = IORegistryIterator::iterateOver(gIODTPlane, kIORegistryIterateRecursively);
    if (!iter) return false;

    OSArray * newIORanges = OSArray::withCapacity(1);
    OSArray * newMemRanges = OSArray::withCapacity(1);
    if (!newIORanges || !newMemRanges) return false;

    OSSymbol * myName = (OSSymbol *)bridgeDevice->copyName();

    IORegistryEntry * entry;
    while (entry = OSDynamicCast(IORegistryEntry, iter->getNextObject())) {

	if (!entry->compareName(myName)) continue;

	// get the address ranges from the Open Firmware "ranges" property.
	// 
	// the "ranges" property is a set of tuples consisting of:
	//       <child-phys parent-phys size>
	    
	extern void IODTGetCellCounts(IORegistryEntry * entry, UInt32 * sizeCount, UInt32 * addressCount);

	UInt32 sizeCells, addressCells, tupleSize;
	IODTGetCellCounts(entry, &sizeCells, &addressCells);

	// add up the cells for child, parent and size cells, this code makes
	// the assumption that since our parent is also pci it's encodes addresses
	// with the same number of cells and the pci based child.
	tupleSize = addressCells + addressCells + sizeCells;
	if (!addressCells || !sizeCells) break;

	OSData * prop = OSDynamicCast(OSData, entry->getProperty("ranges"));
	if (prop) {
	    UInt32 * range = (UInt32 *) prop->getBytesNoCopy();
	    UInt32 * endOfRange = range + ((UInt32)prop->getLength() / sizeof(UInt32));

	    while (range < endOfRange) {

		UInt32 data[2];
		data[0] = IOPhysical32(0, range[addressCells - 1]);
		data[1] = IOPhysical32(0, range[addressCells - 1] + range[tupleSize - 1] - 1);

		UInt32 spaceCode = ((IOPCIAddressSpace *)range)->s.space;

		// OpenFirmware likes to hand out i/o addresses above 0xffff when
		// there is more than one controller, just ignore addresses above 0xffff

		if (spaceCode == kIOPCIIOSpace) {
		    if (data[0] < 0xffff) {
			if (data[1] > 0xffff) data[1] = 0xffff;
			OSData * temp = OSData::withBytes(data, sizeof(UInt32) * 2);
			newIORanges->setObject(temp);
			temp->release();
		    }
		}
		    
		if (spaceCode == kIOPCI32BitMemorySpace) {
		    OSData * temp = OSData::withBytes(data, sizeof(UInt32) * 2);
		    newMemRanges->setObject(temp);
		    temp->release();
		}

		range += tupleSize;
	    }
	}
    }
    iter->release();
    myName->release();

    if ((newIORanges->getCount() == 0) || (newMemRanges->getCount() == 0)) {
	newIORanges->release();
	newMemRanges->release();
	return false;
    }

    *ioRanges = newIORanges;
    *memRanges = newMemRanges;

    return true;
}

bool
IOPCCardBridge::getConfigurationSettings(void)
{
    // Get the settings for this platforms memory, io ports, and irq ranges.
    OSData * modelData = 0;
    IORegistryEntry * root = 0;
    
    IOLockLock(gIOPCCardBridgeLock);
    
    if (gIOPCCardConfigurationInited) {
    
    	gIOPCCardConfigurationInited++;
    } else {

	if((root = IORegistryEntry::fromPath( "/", gIODTPlane ))) {
	    modelData = OSDynamicCast(OSData, root->getProperty("model"));
	    root->release();
	}
	if (!modelData) {
	    IOLog("IOPCCardBridge::getConfigurationSettings: can't find this platform's model name in registry?\n");
	    IOLockUnlock(gIOPCCardBridgeLock); 
	    return false;
	}
	char * modelName = (char *)modelData->getBytesNoCopy();
	
	OSDictionary * config = OSDynamicCast(OSDictionary, getProperty("Configuration Settings"));
	if (!config) {
	    IOLog("IOPCCardBridge::getConfigurationSettings: Configuration Settings is not set?\n");
	    IOLockUnlock(gIOPCCardBridgeLock); 
	    return false;
	}
	OSDictionary * settings = OSDynamicCast(OSDictionary, config->getObject(modelName));

	// the plist settings are meant to make up for machines that don't have a
	// Open Firmware capable of properly describing the needed configuration
	// information (or incorrectly describing it).  Not finding a model in the
	// plist settings should be considered a good thing.

	if (!settings) {
	    if (getOFConfigurationSettings(&gAvailableIORanges, &gAvailableMemRanges)) {
		gIOPCCardConfigurationInited = 1;
		IOLockUnlock(gIOPCCardBridgeLock); 
		return true;
	    }
	    IOLog("IOPCCardBridge::getOFConfigurationSettings: failed to the configure machine\n");
	}
#ifdef PCMCIA_DEBUG
	if (!settings) settings = OSDynamicCast(OSDictionary, config->getObject("Test Machine"));
#endif
	if (!settings) {
	    IOLog("IOPCCardBridge::getConfigurationSettings: unable to configure model name \"%s\".\n", modelName);
	    IOLockUnlock(gIOPCCardBridgeLock); 
	    return false;
	}
    
	gAvailableIORanges = OSDynamicCast(OSArray, settings->getObject("I/O Port Ranges"));
	if (!gAvailableIORanges) {
	    IOLog("IOPCCardBridge::getConfigurationSettings: can't find \"I/O Port Ranges\" property for model name \"%s\" in Configuration Settings?\n");
	    IOLockUnlock(gIOPCCardBridgeLock); 
	    return false;
	}
	gAvailableIORanges->retain();
    
	gAvailableMemRanges = OSDynamicCast(OSArray, settings->getObject("Memory Ranges"));
	if (!gAvailableMemRanges) {
	    IOLog("IOPCCardBridge::getConfigurationSettings: can't find \"Memory Ranges\" property for model name \"%s\" in Configuration Settings?\n");
	    IOLockUnlock(gIOPCCardBridgeLock); 
	    return false;
	}
	gAvailableMemRanges->retain();
    
#ifdef NOTYET
	gAvailableIRQRanges = OSDynamicCast(OSArray, settings->getObject("IRQ Ranges"));
	if (!gAvailableIRQRanges) {
	    IOLog("IOPCCardBridge::getConfigurationSettings: can't find \"IRQ Ranges\" property for model name \"%s\" in Configuration Settings?\n");
	    IOLockUnlock(gIOPCCardBridgeLock); 
	    return false;
	}
	gAvailableIRQRanges->retain();
#endif
	gIOPCCardConfigurationInited = 1;
    }
    
    IOLockUnlock(gIOPCCardBridgeLock); 

    return true;
}

bool
IOPCCardBridge::configureDynamicBridgeSpace(void)
{
    unsigned int	i, max;
    int 		ret = 0;
    OSData * 		range;
    IOPhysicalAddress *	p;
    const unsigned int	maxRangeIndex = 16;	// arbitary
    IOPhysicalRange	ranges[maxRangeIndex];
    
    // setup pci bridge numbering
    if (!wackBridgeBusNumbers(bridgeDevice)) return false;
    
    IOLockLock(gIOPCCardBridgeLock);

    // we only want to do this once
    if (gIOPCCardResourcesInited) {
    
    	gIOPCCardResourcesInited++;
    } else {

	// add memory ranges
	IOPhysicalAddress 	memStart = 0;
	IOPhysicalAddress 	memEnd = 0;
	IOPhysicalLength 	memLength = 0;

	adjust_t adj;
	adj.Action = ADD_MANAGED_RESOURCE;
	adj.Attributes = 0;

	max = gAvailableMemRanges->getCount(); 
	if (max > maxRangeIndex) return false;
	for (i=0; i < max; i++) {
	
	    range = OSDynamicCast(OSData, gAvailableMemRanges->getObject(i));
	    if (!range) return false;
	    if (range->getLength() != 8) return false;
	
	    p = (IOPhysicalAddress *)range->getBytesNoCopy();
	    memStart = *p++;
	    memEnd = *p;
	    memLength = (memEnd + 1) - memStart;
	    
	    DEBUG(1, "adding bridge mem space 0x%x-0x%x\n", memStart, memEnd);

	    if (!addBridgeMemoryRange(memStart, memLength, false)) {
		IOLog("IOPCCardBridge: failed to get bridge memory range 0x%x - 0x%x\n",
		    memStart, memStart + memLength - 1);
		return false;
	    }
    
	    // notify card services of new memory address space
	    adj.Resource = RES_MEMORY_RANGE;
	    adj.resource.memory.Base = memStart;
	    adj.resource.memory.Size = memLength;

	    ret = CardServices(AdjustResourceInfo, CS_ADJUST_FAKE_HANDLE, &adj, NULL);
	    if (ret != CS_SUCCESS) {
		cs_error(NULL, AdjustResourceInfo, ret);
		return false;
	    } 
 
	    ranges[i].address = (IOPhysicalAddress)memStart;
	    ranges[i].length =  (IOByteCount)memLength;
	}

	gDynamicBridgeSpace = (IODeviceMemory *)IOMemoryDescriptor::withPhysicalRanges(ranges, max, kIODirectionNone);

	// add I/O port ranges
	IOPhysicalAddress 	ioStart = 0;
	IOPhysicalAddress 	ioEnd = 0;
	IOPhysicalLength 	ioLength = 0;

	max = gAvailableIORanges->getCount(); 
	if (max > maxRangeIndex) return false;
	for (i=0; i < max; i++) {

	    range = OSDynamicCast(OSData, gAvailableIORanges->getObject(i));
	    if (!range) return false;
	    if (range->getLength() != 8) return false;
	
	    p = (IOPhysicalAddress *)range->getBytesNoCopy();
	    ioStart = *p++;
	    ioEnd = *p;
	    ioLength = (ioEnd + 1) - ioStart;
	    
	    DEBUG(1, "adding bridge io  space 0x%x-0x%x\n", ioStart, ioEnd);
    
	    if (!addBridgeIORange(ioStart, ioLength)) {
		IOLog("IOPCCardBridge: failed to get bridge io range 0x%x - 0x%x\n",
		    ioStart, ioStart + ioLength - 1);
		return false;
	    }
    
    	    // notify card services of new io address space
	    adj.Resource = RES_IO_RANGE;
	    adj.resource.io.BasePort = ioStart & 0x0000ffff;
	    if (ioLength > 0xffff) {
		adj.resource.io.NumPorts = 0xffff;
	    } else {
		adj.resource.io.NumPorts = ioLength;
	    }
	    adj.resource.io.IOAddrLines = 0; // doesn't appear to be used?
    
	    ret = CardServices(AdjustResourceInfo, CS_ADJUST_FAKE_HANDLE, &adj, NULL);
	    if (ret != CS_SUCCESS) {
		IOLockUnlock(gIOPCCardBridgeLock);
		cs_error(NULL, AdjustResourceInfo, ret);
		return false;
	    } 
	    
	    ranges[i].address = (IOPhysicalAddress)ioStart;
	    ranges[i].length =  (IOByteCount)ioLength;
	}

	gDynamicBridgeIOSpace = (IODeviceMemory *)IOMemoryDescriptor::withPhysicalRanges(ranges, max, kIODirectionNone);

	gIOPCCardResourcesInited = 1;
    }
    
    dynamicBridgeSpace = gDynamicBridgeSpace;
    dynamicBridgeSpace->retain();
    dynamicBridgeIOSpace = gDynamicBridgeIOSpace;
    dynamicBridgeIOSpace->retain();

    IOLockUnlock(gIOPCCardBridgeLock);

    return true;
}

IODeviceMemory *
IOPCCardBridge::getDynamicBridgeSpace(void)
{
    return dynamicBridgeSpace;
}

IODeviceMemory *
IOPCCardBridge::getDynamicBridgeIOSpace(void)
{
    return dynamicBridgeIOSpace;
}

//*****************************************************************************

bool
IOPCCardBridge::initializeSocketServices(void)
{
    return init_i82365(this, bridgeDevice, cardBusRegisterMap->getVirtualAddress()) == 0;
}

bool
IOPCCardBridge::initializeDriverServices(void)
{
    client_reg_t client_reg;
    servinfo_t serv;
    bind_req_t bind;
    socket_info_t *s;
    int ret;
    unsigned int i, starting_socket;
    
    DEBUG(0, "%s\n", version);
    
    CardServices(GetCardServicesInfo, &serv, NULL, NULL);
    if (serv.Revision != CS_RELEASE_CODE) {
	printk(KERN_NOTICE "ds: Card Services release does not match!\n");
	return false;
    }
    if ((serv.Count == 0) || (serv.Count == sockets)) {
	printk(KERN_NOTICE "ds: no new sockets found?\n");
	return false;
    }

    starting_socket = sockets;
    sockets = serv.Count;
    if (starting_socket) {
	DEBUG(2, "ds: reallocing socket_table, sockets = %d, starting_socket = %d\n", sockets, starting_socket);
	socket_table = (socket_info_t *)krealloc(socket_table, sockets*sizeof(socket_info_t), GFP_KERNEL);
    } else {
	socket_table = (socket_info_t *)kmalloc(sockets*sizeof(socket_info_t), GFP_KERNEL);
    }
    if (!socket_table) return false;
    for (i = starting_socket, s = socket_table+starting_socket; i < sockets; i++, s++) {
	s->bridge = this;
	s->state = 0;
	s->configHandle = 0;
	bzero(&s->config, sizeof(config_req_t));
	s->removal.data = i;
	s->removal.function = &cardRemovalHandler;
	s->handle = NULL;
	s->nubs = OSArray::withCapacity(1);
    }
    
    /* Set up hotline to Card Services */
    client_reg.dev_info = bind.dev_info = &dev_info;
    client_reg.Attributes = INFO_MASTER_CLIENT;
    client_reg.EventMask = CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
			   CS_EVENT_EJECTION_REQUEST | CS_EVENT_EJECTION_COMPLETE |
			   CS_EVENT_INSERTION_REQUEST | CS_EVENT_PM_RESUME;
    client_reg.event_handler = &cardEventHandler;
    client_reg.Version = 0x0210;

    for (i = starting_socket; i < sockets; i++) {
	bind.Socket = i;
	bind.Function = BIND_FN_ALL;
	ret = CardServices(BindDevice, &bind, NULL, NULL);
	if (ret != CS_SUCCESS) {
	    cs_error(NULL, BindDevice, ret);
	    break;
	}

	client_reg.event_callback_args.client_data = (void *)i;
 	ret = CardServices(RegisterClient, &socket_table[i].handle,
			   &client_reg, NULL);
	if (ret != CS_SUCCESS) {
	    cs_error(NULL, RegisterClient, ret);
	    break;
	}
    }
    
    return true;
}

//*****************************************************************************

OSDictionary * 
IOPCCardBridge::constructPCCard16Properties(IOPCCard16Device *nub)
{
    int		ret;

    client_handle_t handle = nub->getCardServicesHandle();

    OSDictionary * propTable = OSDictionary::withCapacity(8);
    if (!propTable) return 0;

    do {
	OSNumber *numberProp;
	cisinfo_t cisinfo;

	ret = CardServices(ValidateCIS, handle, &cisinfo, NULL);
	if (ret != CS_SUCCESS) {
	    cs_error(handle, ValidateCIS, ret);
	    break;
	} 

	if (cisinfo.Chains > 0) {

	    cistpl_manfid_t manfid;
	    ret = read_tuple(handle, CISTPL_MANFID, &manfid, TUPLE_RETURN_COMMON);
	    if (ret == CS_SUCCESS) {
		numberProp = OSNumber::withNumber(manfid.manf, 32);
		if (!numberProp) break;
		propTable->setObject(kIOPCCardVendorIDMatchKey, numberProp);
		numberProp->release();

		numberProp = OSNumber::withNumber(manfid.card, 32);
		if (!numberProp) break;
		propTable->setObject(kIOPCCardDeviceIDMatchKey, numberProp);
		numberProp->release();

		char nameStr[32];
		sprintf(nameStr, "pccard%x,%x", manfid.manf, manfid.card);
		const OSSymbol *nameProp = OSSymbol::withCString(nameStr);
		if (!nameProp) break;
		propTable->setObject(gIONameKey, (OSSymbol *) nameProp);
		nameProp->release();
	    } else {
		const OSSymbol *nameProp = OSSymbol::withCString("pccard-no-manfid");
		if (!nameProp) break;
		propTable->setObject(gIONameKey, (OSSymbol *) nameProp);
		nameProp->release();
	    }
            
	    cistpl_vers_1_t version_1;
	    if (read_tuple(handle, CISTPL_VERS_1, &version_1, TUPLE_RETURN_COMMON) == CS_SUCCESS && (version_1.ns)) {
		OSArray *vers1 = OSArray::withCapacity(version_1.ns);
		if (!vers1) break;
		propTable->setObject(kIOPCCardVersionOneMatchKey, vers1);
		for (UInt32 i=0; i < version_1.ns; i++) {
		    const OSSymbol *str = OSSymbol::withCString(version_1.str+version_1.ofs[i]);
		    if (!str) break;
		    vers1->setObject(i, (OSSymbol *)str);
		    str->release();
		}
		vers1->release();
	    }

	    cistpl_funcid_t funcid;
	    funcid.func = CISTPL_FUNCID_INVALID; // for below
	    ret = read_tuple(handle, CISTPL_FUNCID, &funcid);
	    if (ret == CS_SUCCESS) {

		numberProp = OSNumber::withNumber(funcid.func, 32);
		if (!numberProp) break;
		propTable->setObject(kIOPCCardFunctionIDMatchKey, numberProp);
		numberProp->release();

		static const char * functionNames[] = {
		    "Multi-Function", "Memory", "Serial Port", "Parallel Port", "Fixed Disk", "Video Adapter",
		    "Network Adapter", "AIMS", "SCSI", "Security", "Instrument"
		};
		const char *funcStr = 0;
		if (funcid.func <= CISTPL_FUNCID_MAX_ID) {
		    funcStr = functionNames[funcid.func];
		} else if (funcid.func == CISTPL_FUNCID_VENDOR) {
		    funcStr = "Vendor-Specific";
		} 
		if (funcStr) {
		    const OSSymbol *funcProp = OSSymbol::withCString(funcStr);
		    if (!funcProp) break;
		    propTable->setObject(kIOPCCardFunctionNameMatchKey, (OSSymbol *)funcProp);
		    funcProp->release();
		}

		// since we found a function id, see if it has any function extension tuples

		OSArray * funceArray = OSArray::withCapacity(10);
		if (!funceArray) break;

		tuple_t tuple;
		cisdata_t buf[255];

		tuple.DesiredTuple = CISTPL_FUNCE;
		tuple.Attributes = TUPLE_RETURN_COMMON;
		ret = CardServices(GetFirstTuple, handle, &tuple, NULL);
		while (ret == CS_SUCCESS) {
		    tuple.TupleData = buf;
		    tuple.TupleOffset = 0;
		    tuple.TupleDataMax = sizeof(buf);
		    ret = CardServices(GetTupleData, handle, &tuple, NULL);
		    if ((ret == CS_SUCCESS) && (tuple.TupleDataLen > 0)) {
			const OSData *funceEntry = OSData::withBytes(tuple.TupleData, tuple.TupleDataLen);
			if (!funceEntry) break;
			funceArray->setObject((OSSymbol *)funceEntry);
			funceEntry->release();
		    }
		    ret = CardServices(GetNextTuple, handle, &tuple, NULL);
		}
		if (funceArray->getCount()) {
		    propTable->setObject(kIOPCCardFunctionExtensionMatchKey, (OSSymbol *)funceArray);
		}
		funceArray->release();
	    }

	    static const char * deviceNames[] = {
		"No Device",	"ROM",		"OTPROM",	"EPROM",
		"EEPROM",	"Flash",	"SRAM",		"DRAM",
		"Reserved_8",	"Reserved_9",	"Reserved_A",	"Reserved_B",
		"Reserved_C",	"Function Specific","Extended","Reserved_F"
	    };

	    // looks like a memory card, so lets check for device and jedec tuples
	    if (funcid.func == CISTPL_FUNCID_MEMORY || funcid.func == CISTPL_FUNCID_INVALID) {
		cistpl_device_t	device;
		ret = read_tuple(handle, CISTPL_DEVICE, &device, TUPLE_RETURN_COMMON);
		if (ret != CS_SUCCESS) {
		    ret = read_tuple(handle, CISTPL_DEVICE_A, &device, TUPLE_RETURN_COMMON);
		}
		// mac os 9 just looks at the first field in device tuple?
		// it ignores the rest of the tuple, lets do the same for now.
		if (ret == CS_SUCCESS) {
		    const char *deviceStr = 0;
		    if (device.ndev > 0) deviceStr = deviceNames[device.dev[0].type];
		    if (deviceStr) {
			const OSSymbol *deviceProp = OSSymbol::withCString(deviceStr);
			if (!deviceProp) break;
			propTable->setObject(kIOPCCardMemoryDeviceNameMatchKey, (OSSymbol *)deviceProp);
			deviceProp->release();
		    }
		}
#ifdef LATERXXX
		// MACOSXXX - we could create mtd nubs for mtd driver matching?
		// MACOSXXX - do we want to get regions here? leave for enabler? leave for driver?
		
		cistpl_jedec_t jedec;
		ret = read_tuple(handle, CISTPL_JEDEC_C, &device);
		if (ret != CS_SUCCESS) {
		    ret = read_tuple(handle, CISTPL_JEDEC_A, &device);
		}
		if (ret == CS_SUCCESS) {
		}
#endif
	    }
	} else {
	    // if we got this far we have already done a validate cis and a get status
	    // so at this point this card is most likely a sram card, but we don't call
	    // it that just in case someone else wants to match against it.

	    const OSSymbol *nameProp = OSSymbol::withCString("pccard-no-cis");
	    if (!nameProp) break;
	    propTable->setObject(gIONameKey, (OSSymbol *) nameProp);
	    nameProp->release();

	    const OSSymbol *funcProp = OSSymbol::withCString("Anonymous");
	    if (!funcProp) break;
	    propTable->setObject(kIOPCCardFunctionNameMatchKey, (OSSymbol *)funcProp);
	    funcProp->release();
	}
    } while (0);

    // copy pmu socket location info into nub
    OSData * socketData = OSDynamicCast(OSData, bridgeDevice->getProperty("AAPL,pmu-socket-number"));
    if (socketData) {
	propTable->setObject("AAPL,pmu-socket-number", socketData);
    }

    // always return propTable even it is empty
    return propTable;
}

OSDictionary * 
IOPCCardBridge::constructCardBusProperties(IOPCIAddressSpace space)
{
    OSDictionary * dict = super::constructProperties(space);

    UInt32 classCode = configRead32(space, kIOPCIConfigClassCode & 0xfc) >> 8;

    OSData *prop = OSData::withBytes( &classCode, 4 );
    if (prop) {
	dict->setObject("class-code", prop );
	prop->release();
    }
    
    // copy pmu socket location info into nub
    OSData * socketData = OSDynamicCast(OSData, bridgeDevice->getProperty("AAPL,pmu-socket-number"));
    if (socketData) {
	dict->setObject("AAPL,pmu-socket-number", socketData);
    }

    return dict;
}

void
IOPCCardBridge::constructCardBusCISProperties(IOCardBusDevice *nub)
{
    client_handle_t handle = nub->getCardServicesHandle();

    cistpl_vers_1_t version_1;
    if (read_tuple(handle, CISTPL_VERS_1, &version_1, TUPLE_RETURN_COMMON) == CS_SUCCESS && (version_1.ns)) {
	OSArray *vers1 = OSArray::withCapacity(version_1.ns);
	if (!vers1) return;
	nub->setProperty(kIOPCCardVersionOneMatchKey, vers1);
	for (UInt32 i=0; i < version_1.ns; i++) {
	    const OSSymbol *str = OSSymbol::withCString(version_1.str+version_1.ofs[i]);
	    if (!str) return;
	    vers1->setObject(i, (OSSymbol *)str);
	    str->release();
	}
	vers1->release();
    }
}

void
IOPCCardBridge::addNubInterruptProperties(OSDictionary * propTable)
{
    int            i;
    OSArray *      controller;
    OSArray *      specifier;
    OSData *       tmpData;
    long           tmpLong;

#define kNumNubVectors 1

    // Create the interrupt specifer array.
    specifier = OSArray::withCapacity(kNumNubVectors);
    assert(specifier);
    for (i = 0; i < kNumNubVectors; i++) {
        tmpLong = i;
        tmpData = OSData::withBytes(&tmpLong, sizeof(tmpLong));
        specifier->setObject(tmpData);
    }

    // Create the interrupt controller array.
    controller = OSArray::withCapacity(kNumNubVectors);
    assert(controller);
    for (i = 0; i < kNumNubVectors; i++)
        controller->setObject(interruptControllerName);

    // Put the two arrays into the property table.
    propTable->setObject(gIOInterruptControllersKey, controller);
    propTable->setObject(gIOInterruptSpecifiersKey, specifier);

    // Release the arrays after being added to the property table.
    specifier->release();
    controller->release();
}

bool
IOPCCardBridge::publishPCCard16Nub(IOPCCard16Device * nub, UInt32 socketIndex, UInt32 functionIndex)
{
    if (nub) {
        nub->setProperty("SocketNumber", socketIndex, 32);
        nub->setProperty("FunctionNumber", functionIndex, 32);

	char location[24];
	sprintf(location, "%X,%X", socketIndex, functionIndex);
        nub->setLocation(location);

        if (nub->attach(this)) {
            nub->registerService();
	    return true;
	}
    }
    return false;
}

bool
IOPCCardBridge::publishCardBusNub(IOCardBusDevice * nub, UInt32 socketIndex, UInt32 functionIndex)
{
    if (nub) {
        nub->setProperty("SocketNumber", socketIndex, 32);
        nub->setProperty("FunctionNumber", functionIndex, 32);
    }

    return super::publishNub(nub, socketIndex);
}

//*****************************************************************************

int
IOPCCardBridge::cardEventHandler(cs_event_t event, int priority,
				 event_callback_args_t *args)
{
    DEBUG(1, "IOPCCardBridge::cardEventHandler(0x%06x, %d, 0x%p)\n",
	  event, priority, args->client_handle);

    int ret;
    int i = (int)args->client_data;
    socket_info_t *s = &socket_table[i];
    
    switch (event) {
	
    case CS_EVENT_CARD_REMOVAL:
	s->state &= ~SOCKET_PRESENT;
	if (!(s->state & SOCKET_REMOVAL_PENDING)) {
	    s->state |= SOCKET_REMOVAL_PENDING;
	    s->removal.expires = jiffies + HZ/10;
	    add_timer(&s->removal);
	}
	break;
	
    case CS_EVENT_CARD_INSERTION:
	s->state |= SOCKET_PRESENT;
	{
	    cs_status_t status;
	    status.Function = 0;
	    ret = CardServices(GetStatus, s->handle, &status, NULL);
	    if (ret != CS_SUCCESS) {
		cs_error(s->handle, GetStatus, ret);
		break;
	    } 

	    int has_cis = 0;
	    cisinfo_t cisinfo;
	    ret = CardServices(ValidateCIS, s->handle, &cisinfo, NULL);
	    if (ret != CS_SUCCESS) cs_error(s->handle, ValidateCIS, ret);
	    DEBUG(2, "ValidateCIS returned 0x%x, chain count = %d\n", ret, cisinfo.Chains);
	    has_cis = ((ret == 0) && (cisinfo.Chains > 0));

	    if (status.CardState & CS_EVENT_CB_DETECT) {

		int count = s->nubs->getCount();

		for (int fn=0; fn < count; fn++) {

		    config_info_t config;
		    config.Function = fn;
		    ret = CardServices(GetConfigurationInfo, s->handle, &config, NULL);
		    if (ret != CS_SUCCESS) {
			cs_error(s->handle, GetConfigurationInfo, ret);
			break;
		    } 
		    IOPCCardBridge *bus = (IOPCCardBridge *)config.PCCardNub;
		    IOCardBusDevice *nub = (IOCardBusDevice *)config.CardBusNub;
		    if (!bus || !nub) continue;

#ifdef PCMCIA_DEBUG
		    // sanity check - is this the right nub?
		    if (nub != s->nubs->getObject(fn)) {
			IOLog("IOPCCardBridge::cardEventHandler: nub %p does not match nubs[%d]?\n", nub, fn);
			continue;
		    }
#endif
		    nub->bindCardServices();

		    if (has_cis) bus->constructCardBusCISProperties(nub);

		    if (!bus->publishCardBusNub(nub, i, fn)) continue;
		}
	    } else {		//must be a 16 bit card

		cistpl_longlink_mfc_t mfc;
		mfc.nfn = 1;
		if (has_cis) {
		    if (read_tuple(s->handle, CISTPL_LONGLINK_MFC, &mfc, TUPLE_RETURN_COMMON) == CS_SUCCESS)
			DEBUG(2, "number of functions %d\n", mfc.nfn);
		}
		
		// MACOSXXX - either need to add support to Card Services, for InquireConfiguration,
		// ConfigureFunction, ... or use the same hack the cardbus code does currently.
		
//		for (int fn=0; fn < mfc.nfn; fn++) {
		for (int fn=0; fn < 1; fn++) {

		    config_info_t config;
		    config.Function = fn;
		    ret = CardServices(GetConfigurationInfo, s->handle, &config, NULL);
		    if (ret != CS_SUCCESS) {
			cs_error(s->handle, GetConfigurationInfo, ret);
			continue;
		    } 
		    IOPCCardBridge *bus = (IOPCCardBridge *)config.PCCardNub;
		    if (!bus) continue;

		    IOPCCard16Device * nub = bus->createPCCard16Nub(i, fn);
		    if (!nub) continue;

		    if (!bus->publishPCCard16Nub(nub, i, fn)) continue;
		}
	    }
	}
	break;

    case CS_EVENT_PM_RESUME:

	if (s->state & SOCKET_CONFIG) {
	    ret = CardServices(RequestConfiguration, s->configHandle, &s->config, NULL);
	    if (ret) cs_error(s->configHandle, RequestConfiguration, ret);
	}

	if (s->bridge) (s->bridge)->acknowledgeSetPowerState();
	break;
    
//    case CS_EVENT_EJECTION_REQUEST:
//	return handle_request(s, event);
//	break;

    case CS_EVENT_EJECTION_COMPLETE:
	{
	    const OSSymbol * matchString = OSSymbol::withCString(kIOPCCardEjectCategory);
	    if (!matchString) break;
    
	    IOService * provider = OSDynamicCast(IOService, s->bridge->getProvider());
	    if (!provider) break;
    
	    IOPCCardEjectController * driver = OSDynamicCast(IOPCCardEjectController,
							    provider->getClientWithCategory(matchString));
	    matchString->release();
	    if (!driver) break;
		
	    driver->ejectCard();
	}
	break;
	
    default:
//	handle_event(s, event);
	break;
    }

    return 0;
}

void
IOPCCardBridge::cardRemovalHandler(u_long sn)
{
    socket_info_t *s = &socket_table[sn];
    s->state &= ~SOCKET_REMOVAL_PENDING;

    IOLog("IOPCCard: shutting down socket %d.\n", sn);

    int count = s->nubs->getCount();
    IOService *nub;

    // call function 0 last
    for (int fn=count - 1; fn >= 0; fn--) {

	nub = OSDynamicCast(IOService, s->nubs->getObject(fn));
 	IOLog("IOPCCard: calling terminate on socket %d function %d nub %p.\n", sn, fn, nub);
	if (nub && !nub->terminate(kIOServiceRequired | kIOServiceSynchronous)) {
	    IOLog("IOPCCard: terminate failed on nub 0x%x.\n", nub);
	}

	s->nubs->removeObject(fn);
    }

    if (s->state) {
	IOLog("IOPCCard: socket %d did not terminate cleanly, state = 0x%x.\n", sn, s->state);
	s->state = 0;
	s->configHandle = 0;
	bzero(&s->config, sizeof(config_req_t));
    }
}

int
IOPCCardBridge::cardServicesGate(IOService *, void *func, void * arg1, void * arg2, void * arg3)
{
    int function = (int)func;

    if (function >= 0) {
	return CardServices(function, arg1, arg2, arg3);
    } else {
	switch (function) {
	case kCSGateProbeBridge: {

	    IOPCCardBridge *bridge = OSDynamicCast(IOPCCardBridge, (OSObject *)arg1);
	    if (!bridge) break;

	    if (!bridge->initializeSocketServices()) break;
	    if (!bridge->initializeDriverServices()) break;
	    return 0;
	}
	case kCSGateTimerCallout: {

	    struct timer_list * timer = (struct timer_list *)arg1;
	    (*timer->function)(timer->data);
	    return 0;
	}
	case kCSGateSetBridgePower: {

	    IOPCCardBridge *bridge = OSDynamicCast(IOPCCardBridge, (OSObject *)arg1);
	    if (!bridge) break;

	    return bridge->setBridgePowerState((unsigned long)arg2, (IOService *)arg3);
	}
	default:
	    break;
	}
    }

    return -1;
}

void
IOPCCardBridge::interruptDispatcher(void)
{
    DEBUG(4, "***** Entering IOPCCardBridge::interruptDispatcher\n");

    interrupt_handler_t * h = interruptHandlers;
    while (h) {
	h->top_handler(h->socket);
	h = h->next;
    }

    DEBUG(4, "***** Exiting IOPCCardBridge::interruptDispatcher\n");
}

//*****************************************************************************
//			c shim interface
//*****************************************************************************

IOPCCard16Device *
IOPCCardBridge::createPCCard16Nub(UInt8 socket, UInt8 function)
{
    OSDictionary * propTable = 0;
    bool isBound = false;

    IOPCCard16Device *nub = new IOPCCard16Device;
    if (!nub) return 0;

    nub->socket = socket;
    nub->function = function;

    do {
	if (!nub->bindCardServices()) break;
	isBound = true;

	propTable = constructPCCard16Properties(nub);
	if (!propTable) break;

	if (ioDeviceMemory())
	    nub->ioMap = ioDeviceMemory()->map();

	addNubInterruptProperties(propTable);

	if (!nub->init(propTable)) break;

	socket_table[socket].nubs->setObject(nub);
	nub->release();

	return nub;

    } while (false);
    
    if (isBound) nub->unbindCardServices();
    if (propTable) propTable->release();
    nub->release();
    return 0;
}

IOCardBusDevice *
IOPCCardBridge::createCardBusNub(UInt8 socket, UInt8 function)
{
    IOPCIAddressSpace	space;

    space.bits = 0;
    space.s.busNum = firstBusNum();
    space.s.deviceNum = 0;
    space.s.functionNum = function;

    UInt32 vendor = configRead32(space, 0) & 0x0000ffff;
    DEBUG(2, "IOPCCardBridge: createCardBusNub socket %d, function %d, vendor id = 0x%x\n", socket, function, vendor);
    if ((vendor == 0) || (vendor == 0xffff)) {
	// this is perfectly valid when scanning multi-function cards
        return 0;
    }

    OSDictionary * propTable = constructCardBusProperties(space);
    if (propTable) {
	IOCardBusDevice *nub = new IOCardBusDevice;
	if (!nub) {
	    propTable->release();
	    return 0;
	}
	nub->socket = socket;
	nub->function = function;

	spaceFromProperties(propTable, &((struct IOCardBusDevice *)nub)->space);
	nub->parent = this;

	if (ioDeviceMemory())
	    nub->ioMap = ioDeviceMemory()->map();

	addNubInterruptProperties(propTable);

	if (!nub->init(propTable)) propTable->release();

	socket_table[socket].nubs->setObject(nub);
	nub->release();

	return nub;
    }
    return 0;
}

bool
IOPCCardBridge::addCSCInterruptHandler(unsigned int socket, unsigned int irq, 
				       int (*top_handler)(int), int (*bottom_handler)(int), 
				       int (*enable_functional)(int), int (*disable_functional)(int),
				       const char* name)
{
    struct interrupt_handler * h = (struct interrupt_handler *)IOMalloc(sizeof(interrupt_handler_t));
    if (!h) return false;

    h->socket = socket;
    h->irq = irq;
    h->top_handler = top_handler;
    h->bottom_handler = bottom_handler;
    h->enable_functional = enable_functional;
    h->disable_functional = disable_functional;
    h->name = name;
    h->interruptSource = interruptSource;

    bridgeDevice->disableInterrupt(0);

    h->next = interruptHandlers;
    interruptHandlers = h;

    interruptController->updateCSCInterruptHandlers(interruptHandlers);

    bridgeDevice->enableInterrupt(0);

    return true;
}

bool
IOPCCardBridge::removeCSCInterruptHandler(unsigned int socket)
{
    interrupt_handler_t * prev = NULL; // shutup compiler
    interrupt_handler_t * h = interruptHandlers;

    bridgeDevice->disableInterrupt(0);

    while (h) {
	if (h->socket == socket) {
	    if (h == interruptHandlers) {
		interruptHandlers = h->next;
	    } else {
		prev->next = h->next;
	    }
	    IOFree(h, sizeof(interrupt_handler_t));
	}

	prev = h;
	h = h->next;
    }

    interruptController->updateCSCInterruptHandlers(interruptHandlers);

    bridgeDevice->enableInterrupt(0);

    return true;
}

//*****************************************************************************
//				public interfaces
//*****************************************************************************

IOService * 
IOPCCardBridge::probe(IOService * provider, SInt32 * score)
{
    if (!getModuleParameters()) return 0;

    DEBUG(2, "IOPCCardBridge::probe\n");

    bridgeDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!bridgeDevice) return 0;

    // OF uses the "ranges" property differently when we have a
    // pci expansion chassis attached, just skip looking at it
    pciExpansionChassis = hasPCIExpansionChassis(provider);
    if (!pciExpansionChassis) {
	if (!getConfigurationSettings()) return 0;
    }

    return super::probe(provider, score);
}

bool
IOPCCardBridge::start(IOService * provider)
{
    IOReturn	error;
	
    DEBUG(2, "IOPCCardBridge::start\n");

    bridgeDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!bridgeDevice) return false;

    // if there is a pci expansion chassis attached, just act like a pci to pci bridge
    // and do nothing, open firmware has already probed and configured the bus
    pciExpansionChassis = hasPCIExpansionChassis(provider);
    if (pciExpansionChassis) {
    
	bool status = super::start(provider);
	if (status != false) {
	    // well, almost do nothing...
	    bridgeDevice->configWrite32 (0x8c, 0x00000002);	// Multifunction Routing {INTA}
	}
	return status;
    }

    do {
	bool success = false;
    
	IOLockLock(gIOPCCardBridgeLock);
	if (gIOPCCardGlobalsInited) {
	
	    gIOPCCardGlobalsInited++;
	    success = true;
	} else {

	    gIOPCCardMappedBridgeSpace = OSSet::withCapacity(16);
	    gIOPCCardWorkLoop = IOWorkLoop::workLoop();
	    gIOPCCardCommandGate = IOCommandGate::commandGate((OSObject *)0x50434344, (IOCommandGate::Action)cardServicesGate);

	    success = gIOPCCardMappedBridgeSpace && gIOPCCardWorkLoop && gIOPCCardCommandGate;
	    success &= !gIOPCCardWorkLoop->addEventSource(gIOPCCardCommandGate);

	    if (success) gIOPCCardGlobalsInited = 1;
	}
	IOLockUnlock(gIOPCCardBridgeLock);

	if (!success) break;
	
	// tell PCI Family to not cache our config space registers
	bridgeDevice->setProperty(kIOPMPCIConfigSpaceVolatileKey, kOSBooleanFalse);

	// mark config space backup as unused
	bridgeConfig[0] = 0;

	// this ends up calling configure
	if (!super::start(provider)) break;
	
	// set current pm state
	changePowerStateTo(kIOPCIDeviceOnState);

	// Allocate the interruptController instance.
	interruptController = new IOPCCardInterruptController;
	if (interruptController == NULL) break;
  
	// call the interruptController's init method.
	error = interruptController->initInterruptController(provider);
	if (error != kIOReturnSuccess) break;
  
	// install interruptController
	IOInterruptAction handler = interruptController->getInterruptHandlerAddress();
	provider->registerInterrupt(0, interruptController, handler, 0);

	// Construct the unique name for this interrupt controller
	UInt32 uniqueNumber = (bridgeDevice->getBusNumber() << 16) |
			      (bridgeDevice->getDeviceNumber() << 8) |
			       bridgeDevice->getFunctionNumber();
	char * uniqueName = kInterruptControllerNamePrefix "12345678";
	sprintf(uniqueName, "%s%08x", kInterruptControllerNamePrefix, uniqueNumber);
	interruptControllerName = (OSSymbol *)OSSymbol::withCString(uniqueName);
	if (!interruptControllerName) break;

	// Register the interrupt controller so clients can find it.
	getPlatform()->registerInterruptController(interruptControllerName, interruptController);
	interruptControllerName->release();

	// install CSC interrupt handler (behind workloop) into interruptController
	interruptSource = IOInterruptEventSource::interruptEventSource((OSObject *)		this,
								       (IOInterruptEventAction)&IOPCCardBridge::interruptDispatcher,
								       (IOService *)		0,
								       (int) 			0);
	if (!interruptSource) break;
	if (gIOPCCardWorkLoop->addEventSource(interruptSource)) break;

	// Finally, enable interrupts
	bridgeDevice->enableInterrupt(0);

	// configure bridge, notify CS of dynamic address spaces
	configureDynamicBridgeSpace();

	// enable card's PCI response
	bridgeDevice->setMemoryEnable(true);
	bridgeDevice->setIOEnable(true);
	bridgeDevice->setBusMasterEnable(true);

	// map in the CardBus/ExCA Registers
	cardBusRegisterMap = bridgeDevice->mapDeviceMemoryWithRegister(0x10);
	if (!cardBusRegisterMap) break;

	// kick off probe inside workloop
	error = gIOPCCardCommandGate->runCommand((void *)kCSGateProbeBridge, (void *)this, NULL, NULL) == 0;
	
	// make us visible to user land
	registerService();
	
	return error;

    } while (false);

    IOLog("IOPCCardBridge::start failed\n");

    stop(provider);

    return false;
}

bool 
IOPCCardBridge::configure(IOService * provider)
{
    DEBUG(2, "IOPCCardBridge::configure\n");

//	this is wrong for cardbus controllers
//	return super::configure(provider);
    
    return true;
}

void
IOPCCardBridge::stop(IOService * provider)
{
    DEBUG(2, "IOPCCardBridge::stop\n");

    if (dynamicBridgeSpace)	dynamicBridgeSpace->release();
    if (dynamicBridgeIOSpace)	dynamicBridgeIOSpace->release();
    if (cardBusRegisterMap)	cardBusRegisterMap->release();	

    if (interruptSource) {
	gIOPCCardWorkLoop->removeEventSource(interruptSource);
    	interruptSource->release();
    }
    if (interruptController) {
    	    bridgeDevice->unregisterInterrupt(0);
	    interruptController->release();
    }

    super::stop(provider);
}

IOReturn
IOPCCardBridge::setPowerState(unsigned long powerState,
			      IOService * whatDevice)
{
    DEBUG(1, "IOPCCardBridge::setPowerState state=%d\n", powerState);

    if (pciExpansionChassis)
	return super::setPowerState(powerState, whatDevice);  // NOOP

    return gIOPCCardCommandGate->runCommand((void *)kCSGateSetBridgePower, (void *)this, (void *)powerState, (void *)whatDevice);
}

/*
 *	turn off power to all cards on this bridge
 */

IOReturn
IOPCCardBridge::setBridgePowerState(unsigned long powerState,
				    IOService * whatDevice)
{
    int ret;

    DEBUG(1, "IOPCCardBridge::setBridgePowerState state=%d\n", powerState);

    // save/restore bridge state
    for (unsigned cnt = 0; cnt < kIOPCCardBridgeRegCount; cnt++)
    {
	if (powerState == kIOPCIDeviceOnState) {
	    if (bridgeConfig[0] != 0)
		bridgeDevice->configWrite32(cnt * 4, bridgeConfig[cnt]);
	}
	if (powerState == kIOPCIDeviceOffState) {
	    bridgeConfig[cnt] = bridgeDevice->configRead32(cnt * 4);
	}
    }

    // for all sockets on this bridge do
    for (unsigned i=0; i < sockets; i++) {
	socket_info_t *s = &socket_table[i];
	if (s->bridge != this) continue;

	if (powerState == kIOPCIDeviceOnState) {
	    if (!(s->state & SOCKET_SUSPEND)) return IOPMAckImplied;
	    s->state &= ~SOCKET_SUSPEND;

	    bridgeDevice->enableInterrupt(0);

	    ret = CardServices(ResumeCard, s->handle, NULL, NULL);
	    if (ret) cs_error(s->configHandle, ResumeCard, ret);

	    if (s->state & SOCKET_PRESENT) {
		// return the max time to power up in usec
		// CS can take over to 5 seconds to reset a card in
		// the worse case, add another second for scheduler delays
		return 6000000;
	    }

	} else if (powerState == kIOPCIDeviceOffState) {
	    if (s->state & SOCKET_SUSPEND) return IOPMAckImplied;
	    s->state |= SOCKET_SUSPEND;
	    
	    if (s->state & SOCKET_CONFIG) {
		ret = CardServices(ReleaseConfiguration, s->configHandle, &s->config, NULL);
		if (ret) cs_error(s->configHandle, ReleaseConfiguration, ret);
	    }

	    if (s->state & SOCKET_PRESENT) {
		ret = CardServices(SuspendCard, s->handle, NULL, NULL);
		if (ret) cs_error(s->configHandle, SuspendCard, ret);
	    }
	    
	    // disable interrupts over sleep
	    bridgeDevice->disableInterrupt(0);
	}
    }
    return IOPMAckImplied;
}

static socket_info_t *
getSocketInfo(IOService * nub)
{
    for (unsigned i=0; i < sockets; i++) {
	OSArray *nubs = socket_table[i].nubs;
	if (!nubs) return 0;
	int j = 0;
	while (IOService *funcNub = (IOService *)nubs->getObject(j++)) {
	    if (nub == funcNub) return &socket_table[i]; 
	}
    }
    return 0;
}

int
IOPCCardBridge::configureSocket(IOService * nub, config_req_t * configuration)
{
    DEBUG(1, "IOPCCardBridge::configureSocket nub=%p\n", nub);

    socket_info_t *s = getSocketInfo(nub);
    if (!s) return CS_BAD_HANDLE;
    if (s->state & SOCKET_CONFIG) return CS_CONFIGURATION_LOCKED;

    client_handle_t handle = 0;
    if (IOCardBusDevice *cardBusNub = OSDynamicCast(IOCardBusDevice, (OSObject *)nub)) {
	handle = cardBusNub->getCardServicesHandle();
    } else if (IOPCCard16Device *pccard16Nub = OSDynamicCast(IOPCCard16Device, (OSObject *)nub)) {
	handle = pccard16Nub->getCardServicesHandle();
    }
    if (!handle) return CS_BAD_HANDLE;

    s->state |= SOCKET_CONFIG;

    int ret = CardServices(RequestConfiguration, handle, configuration, NULL);
    if (ret) {
	cs_error(handle, RequestConfiguration, ret);
	return ret;
    }
    
    //copy configuration locally for sleep/wakeup
    s->config = *configuration;
    s->configHandle = handle;
    
    DEBUG(1, "IOPCCardBridge::configureSocket completed successfully\n");

    return CS_SUCCESS;
}

int
IOPCCardBridge::unconfigureSocket(IOService * nub)
{
    DEBUG(1, "IOPCCardBridge::unconfigureSocket nub=%p\n", nub);

    socket_info_t *s = getSocketInfo(nub);
    if (!s) return CS_BAD_HANDLE;
    if (!(s->state & SOCKET_CONFIG)) return CS_BAD_HANDLE;	// we should be configured
    if (s->state & SOCKET_SUSPEND) return CS_BUSY;		// we shouldn't be suspended

    client_handle_t handle = 0;
    if (IOCardBusDevice *cardBusNub = OSDynamicCast(IOCardBusDevice, (OSObject *)nub)) {
	handle = cardBusNub->getCardServicesHandle();
    } else if (IOPCCard16Device *pccard16Nub = OSDynamicCast(IOPCCard16Device, (OSObject *)nub)) {
	handle = pccard16Nub->getCardServicesHandle();
    }
    if (!handle) return CS_BAD_HANDLE;

    int ret = CardServices(ReleaseConfiguration, handle, &s->config, NULL);
    if (ret) {
	cs_error(handle, ReleaseConfiguration, ret);
	return ret;
    }

    s->state &= ~SOCKET_CONFIG;
    s->configHandle = 0;
    bzero(&s->config, sizeof(config_req_t));
    
    DEBUG(1, "IOPCCardBridge::unconfigureSocket completed successfully\n");

    return CS_SUCCESS;
}

IOWorkLoop * 
IOPCCardBridge::getWorkLoop() const
{ 
    return gIOPCCardWorkLoop ? gIOPCCardWorkLoop : super::getWorkLoop();
}

int
IOPCCardBridge::requestCardEjection(IOService * bridgeDevice)
{
    // do we need locking here?

    for (unsigned i = 0; i < sockets; i++) {

	if (socket_table[i].handle &&
	    socket_table[i].bridge && 
	    socket_table[i].bridge->getProvider() == bridgeDevice) {
		
		return gIOPCCardCommandGate->runCommand((void *)EjectCard, socket_table[i].handle, 0, 0);
	}
    }
    return CS_BAD_ADAPTER;
}


//*****************************************************************************
//*****************************************************************************

#undef  super
#define super IOInterruptController

OSDefineMetaClassAndStructors(IOPCCardInterruptController, IOInterruptController);

OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  0);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  1);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  2);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  3);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  4);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  5);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  6);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  7);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  8);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController,  9);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController, 10);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController, 11);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController, 12);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController, 13);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController, 14);
OSMetaClassDefineReservedUnused(IOPCCardInterruptController, 15);

// 2 sockets * 8 functions per socket
#define kNumVectors (16)

IOReturn
IOPCCardInterruptController::initInterruptController(IOService *provider)
{
    int cnt;
  
    IOPCIDevice *bridgeDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!bridgeDevice) {
	IOLog("IOPCCardInterruptController::initInterruptController failed\n");
	return kIOReturnBadArgument;
    }
    
//  registeredEvents = 0;
  
    // Allocate the memory for the vectors
    vectors = (IOInterruptVector *)IOMalloc(kNumVectors * sizeof(IOInterruptVector));
    if (vectors == NULL) return kIOReturnNoMemory;
    bzero(vectors, kNumVectors * sizeof(IOInterruptVector));
  
    // Allocate locks for the
    for (cnt = 0; cnt < kNumVectors; cnt++) {
	vectors[cnt].interruptLock = IOLockAlloc();
	if (vectors[cnt].interruptLock == NULL) {
	    for (cnt = 0; cnt < kNumVectors; cnt++) {
		if (vectors[cnt].interruptLock != NULL)
		    IOLockFree(vectors[cnt].interruptLock);
	    }
	    return kIOReturnNoResources;
	}
    }
  
    return kIOReturnSuccess;
}

IOInterruptAction
IOPCCardInterruptController::getInterruptHandlerAddress(void)
{
    return (IOInterruptAction)&IOPCCardInterruptController::handleInterrupt;
}

#ifdef __ppc__
#define sync() __asm__ volatile("sync")
#define isync() __asm__ volatile("isync")
#endif

IOReturn
IOPCCardInterruptController::handleInterrupt(void */*refCon*/,
					     IOService */*nub*/,
					     int /*source*/)
{
    long              vectorNumber;
    IOInterruptVector *vector;

    // this is for the bridges CSC interrupt handler
    interrupt_handler_t * h = interruptHandlers;
    while (h) {
	unsigned int event = h->bottom_handler(h->socket);
	if (event) h->interruptSource->interruptOccurred(0, 0, 0);
	h = h->next;
    }

    pendingEvents = 0;

    for (vectorNumber=0; vectorNumber < kNumVectors;  vectorNumber++) {

//	if (!(registeredEvents & (1 << vectorNumber))) continue;

	vector = &vectors[vectorNumber];
	vector->interruptActive = 1;
#ifdef __ppc__
	sync();
	isync();
#endif
	if (!vector->interruptDisabledSoft) {
#ifdef __ppc__
	    isync();
#endif
	    // Call the handler if it exists.
	    if (vector->interruptRegistered) {

		vector->handler(vector->target, vector->refCon,
				vector->nub, vector->source);
//	    } else {
//		registeredEvents ^= (1 << vectorNumber);
	    }
	} else {
	    // Hard disable the source.
	    vector->interruptDisabledHard = 1;
	    disableVectorHard(vectorNumber, vector);
	}
      
	vector->interruptActive = 0;
    }
  
    return kIOReturnSuccess;
}

//void
//IOPCCardInterruptController::initVector(long vectorNumber, IOInterruptVector */*vector*/)
//{
//    registeredEvents |= (1 << vectorNumber);
//}

bool
IOPCCardInterruptController::vectorCanBeShared(long /*vectorNumber*/, IOInterruptVector */*vector*/)
{
    return true;
}

int
IOPCCardInterruptController::getVectorType(long /*vectorNumber*/,
					  IOInterruptVector */*vector*/)
{
    return kIOInterruptTypeLevel;
}

void
IOPCCardInterruptController::disableVectorHard(long vectorNumber, IOInterruptVector */*vector*/)
{
    interrupt_handler_t * h = interruptHandlers;
    while (h) {
	h->disable_functional(h->socket);
	h = h->next;
    }
}

void
IOPCCardInterruptController::enableVector(long vectorNumber,
					  IOInterruptVector *vector)
{
    interrupt_handler_t * h = interruptHandlers;
    while (h) {
	h->enable_functional(h->socket);
	h = h->next;
    }
}

void
IOPCCardInterruptController::causeVector(long vectorNumber,
					 IOInterruptVector */*vector*/)
{
    pendingEvents |= 1 << vectorNumber;
    bridgeDevice->causeInterrupt(0);
}


bool
IOPCCardInterruptController::updateCSCInterruptHandlers(interrupt_handler_t *handlers)
{
    interruptHandlers = handlers;
    return true;
}

// yes, this is a hack!
IOReturn
IOPCCardInterruptController::getInterruptType(int source, int *interruptType)
{
    *interruptType = getVectorType(0 , 0);
    return kIOReturnSuccess;
}
