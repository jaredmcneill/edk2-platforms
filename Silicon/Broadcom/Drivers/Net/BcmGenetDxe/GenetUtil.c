/** @file
  Copyright (c) 2020 Jared McNeill. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause
**/

#include "Genet.h"
#include "GenetReg.h"
#include "PhyReg.h"

#include <Library/ArmLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define __LOWEST_SET_BIT(__mask)    ((((__mask) - 1) & (__mask)) ^ (__mask))
#define __SHIFTOUT(__x, __mask)     (((__x) & (__mask)) / __LOWEST_SET_BIT(__mask))
#define __SHIFTIN(__x, __mask)      ((__x) * __LOWEST_SET_BIT(__mask))

#define GENET_PHY_RETRY             1000
#define GENET_PHY_DELAY             10
#define GENET_PHY_RESET_TIMEOUT     500
#define GENET_PHY_LINK_TIMEOUT      200
#define GENET_PHY_ANEG_TIMEOUT      200

UINT32
EFIAPI
GenetMmioRead (
    IN GENET_PRIVATE_DATA * Genet,
    IN UINT32               Offset
    )
{
    ASSERT((Offset & 3) == 0);

    return MmioRead32 (Genet->RegBase + Offset);
}

VOID
EFIAPI
GenetMmioWrite (
    IN GENET_PRIVATE_DATA * Genet,
    IN UINT32               Offset,
    IN UINT32               Data
    )
{
    ASSERT((Offset & 3) == 0);

    MmioWrite32 (Genet->RegBase + Offset, Data);
    ArmDataMemoryBarrier ();
}

STATIC
EFI_STATUS
GenetPhyRead (
    IN GENET_PRIVATE_DATA * Genet,
    IN UINT8                PhyAddr,
    IN UINT8                Reg,
    OUT UINT16 *            Data
    )
{
    UINTN Retry;
    UINT32 Value;

    Value = GENET_MDIO_READ |
            GENET_MDIO_START_BUSY |
            __SHIFTIN(PhyAddr, GENET_MDIO_PMD) |
            __SHIFTIN(Reg, GENET_MDIO_REG);
    GenetMmioWrite (Genet, GENET_MDIO_CMD, Value);

    for (Retry = GENET_PHY_RETRY; Retry > 0; Retry--) {
        Value = GenetMmioRead (Genet, GENET_MDIO_CMD);
        if ((Value & GENET_MDIO_START_BUSY) == 0) {
            *Data = Value & 0xffff;
            break;
        }
        gBS->Stall (GENET_PHY_DELAY);
    }

    if (Retry == 0) {
        DEBUG ((EFI_D_ERROR, "GenetPhyRead: Timeout reading PhyAddr %d, Reg %d\n", PhyAddr, Reg));
        return EFI_TIMEOUT;
    }

    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GenetPhyWrite (
    IN GENET_PRIVATE_DATA * Genet,
    IN UINT8                PhyAddr,
    IN UINT8                Reg,
    IN UINT16               Data
    )
{
    UINTN Retry;
    UINT32 Value;

    Value = GENET_MDIO_WRITE |
            GENET_MDIO_START_BUSY |
            __SHIFTIN(PhyAddr, GENET_MDIO_PMD) |
            __SHIFTIN(Reg, GENET_MDIO_REG);
    GenetMmioWrite (Genet, GENET_MDIO_CMD, Value | Data);

    for (Retry = GENET_PHY_RETRY; Retry > 0; Retry--) {
        Value = GenetMmioRead (Genet, GENET_MDIO_CMD);
        if ((Value & GENET_MDIO_START_BUSY) == 0) {
            break;
        }
        gBS->Stall (GENET_PHY_DELAY);
    }

    if (Retry == 0) {
        DEBUG ((EFI_D_ERROR, "GenetPhyRead: Timeout writing PhyAddr %d, Reg %d\n", PhyAddr, Reg));
        return EFI_TIMEOUT;
    }

    return EFI_SUCCESS;
}

STATIC
EFI_STATUS
GenetPhyDetect (
    IN GENET_PRIVATE_DATA * Genet
    )
{
    EFI_STATUS Status;
    UINT8 PhyAddr;
    UINT16 Id[2];

    for (PhyAddr = 0; PhyAddr < 32; PhyAddr++) {
        Status = GenetPhyRead (Genet, PhyAddr, MII_PHYIDR1, &Id[0]);
        if (EFI_ERROR (Status)) {
            continue;
        }
        Status = GenetPhyRead (Genet, PhyAddr, MII_PHYIDR2, &Id[1]);
        if (EFI_ERROR (Status)) {
            continue;
        }
        if (Id[0] != 0xffff && Id[1] != 0xffff) {
            Genet->PhyAddr = PhyAddr;
            DEBUG ((DEBUG_INFO, "GenetPhyDetect: PHY detected at address 0x%02X (PHYIDR1=0x%04X PHYIDR2=0x%04X)\n",
                    PhyAddr, Id[0], Id[1]));
            return EFI_SUCCESS;
        }
    }

    return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
GenetPhyAutoNegotiate (
    IN GENET_PRIVATE_DATA * Genet
    )
{
    EFI_STATUS Status;
    UINT16 Anar, Gtcr, Bmcr;

    Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_ANAR, &Anar);
    if (EFI_ERROR (Status)) {
        return Status;
    }
    Anar |= ANAR_TX_FD | ANAR_TX | ANAR_10_FD | ANAR_10;
    Status = GenetPhyWrite (Genet, Genet->PhyAddr, MII_ANAR, Anar);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_GTCR, &Gtcr);
    if (EFI_ERROR (Status)) {
        return Status;
    }
    Gtcr |= GTCR_ADV_1000TFDX | GTCR_ADV_1000THDX;
    Status = GenetPhyWrite (Genet, Genet->PhyAddr, MII_GTCR, Gtcr);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_BMCR, &Bmcr);
    if (EFI_ERROR (Status)) {
        return Status;
    }
    Bmcr |= BMCR_AUTOEN | BMCR_STARTNEG;
    return GenetPhyWrite (Genet, Genet->PhyAddr, MII_BMCR, Bmcr);
}

STATIC
EFI_STATUS
GenetPhyGetLinkStatus (
    IN GENET_PRIVATE_DATA * Genet
    )
{
    EFI_STATUS Status;
    UINT16 Bmsr;
    UINTN Retry;

    Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_BMSR, &Bmsr);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    if ((Bmsr & BMSR_LINK) != 0) {
        // Link is up
        return EFI_SUCCESS;
    }

    // Wait for link to come up
    for (Retry = GENET_PHY_LINK_TIMEOUT; Retry > 0; Retry--) {
        Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_BMSR, &Bmsr);
        if (EFI_ERROR (Status)) {
            return Status;
        }

        if ((Bmsr & BMSR_LINK) != 0) {
            // Link is up
            break;
        }

        gBS->Stall (1000);
    }
    if (Retry == 0) {
        // Link is down
        //DEBUG ((EFI_D_ERROR, "GenetPhyGetLinkStatus: Link is down (BMSR=0x%04X)\n", Bmsr));
        return EFI_TIMEOUT;
    }

    // Wait for auto negotiate to complete
    for (Retry = GENET_PHY_ANEG_TIMEOUT; Retry > 0; Retry--) {
        Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_BMSR, &Bmsr);
        if (EFI_ERROR (Status)) {
            return Status;
        }

        if ((Bmsr & BMSR_ACOMP) != 0) {
            // Auto negotiation complete
            break;
        }

        gBS->Stall (1000);
    }
    if (Retry == 0) {
        DEBUG ((EFI_D_ERROR, "GenetPhyGetLinkStatus: Error! Auto negotiation timeout (BMSR=0x%04X)\n", Bmsr));
        return EFI_TIMEOUT;
    }

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
GenetPhyReset (
    IN GENET_PRIVATE_DATA * Genet
    )
{
    EFI_STATUS Status;
    UINTN Retry;
    UINT16 Data;

    // Start reset sequence
    Status = GenetPhyWrite (Genet, Genet->PhyAddr, MII_BMCR, BMCR_RESET);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    // Wait up to 500ms for it to complete
    for (Retry = GENET_PHY_RESET_TIMEOUT; Retry > 0; Retry--) {
        Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_BMCR, &Data);
        if (EFI_ERROR (Status)) {
            return Status;
        }
        if ((Data & BMCR_RESET) == 0) {
            break;
        }
        gBS->Stall (1000);
    }
    if (Retry == 0) {
        return EFI_TIMEOUT;
    }

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
GenetPhyInit (
    IN GENET_PRIVATE_DATA * Genet
    )
{
    EFI_STATUS Status;

    Status = GenetPhyDetect (Genet);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    Status = GenetPhyReset (Genet);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    return GenetPhyAutoNegotiate (Genet);
}

STATIC
EFI_STATUS
GenetPhyGetConfig (
    IN GENET_PRIVATE_DATA * Genet,
    OUT UINT16 *            Speed,
    OUT UINT16 *            Duplex
    )
{
    EFI_STATUS Status;
    UINT16 Gtcr, Gtsr, Anlpar, Anar;
    UINT16 Gt, An;

    Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_GTCR, &Gtcr);
    if (EFI_ERROR (Status)) {
        return Status;
    }
    Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_GTSR, &Gtsr);
    if (EFI_ERROR (Status)) {
        return Status;
    }
    Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_ANLPAR, &Anlpar);
    if (EFI_ERROR (Status)) {
        return Status;
    }
    Status = GenetPhyRead (Genet, Genet->PhyAddr, MII_ANAR, &Anar);
    if (EFI_ERROR (Status)) {
        return Status;
    }

    Gt = Gtsr & (Gtcr << 2);
    An = Anlpar & Anar;

    DEBUG ((EFI_D_INFO, "GenetPhyGetConfig: Gtsr=0x%04X Gtcr=0x%04X Gt=0x%04X\n", Gtsr, Gtcr, Gt));
    DEBUG ((EFI_D_INFO, "GenetPhyGetConfig: Anlpar=0x%04X Anar=0x%04X An=0x%04X\n", Anlpar, Anar, An));

    if ((Gt & (GTCR_ADV_1000TFDX|GTCR_ADV_1000THDX)) != 0) {
        *Speed = BMCR_S1000;
        *Duplex = (Gt & GTCR_ADV_1000TFDX) ? BMCR_FDX : 0;
    } else if ((An & (ANAR_TX_FD|ANAR_TX)) != 0) {
        *Speed = BMCR_S100;
        *Duplex = (An & ANAR_TX_FD) ? BMCR_FDX : 0;
    } else {
        *Speed = BMCR_S10;
        *Duplex = (An & ANAR_10_FD) ? BMCR_FDX : 0;
    }

    DEBUG ((EFI_D_INFO, "GenetPhyGetConfig: Link speed %d Mbps, %a-duplex\n",
           ((*Speed == BMCR_S1000) ? 1000 : (*Speed == BMCR_S100) ? 100 : 10),
           *Duplex == BMCR_FDX ? "full" : "half"));

    return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
GenetPhyUpdateConfig (
    IN GENET_PRIVATE_DATA * Genet
    )
{
    EFI_STATUS Status;
    UINT16 Speed, Duplex;
    BOOLEAN LinkUp;

    Status = GenetPhyGetLinkStatus (Genet);
    LinkUp = EFI_ERROR (Status) ? FALSE : TRUE;

    if (Genet->PhyLinkUp != LinkUp) {
        if (LinkUp) {
            DEBUG ((EFI_D_INFO, "GenetPhyUpdateConfig: Link is up\n"));

            Status = GenetPhyGetConfig (Genet, &Speed, &Duplex);
            if (EFI_ERROR (Status)) {
                return Status;
            }
            //GenetUpdateConfig (Genet, Speed, Duplex);
        } else {
            DEBUG ((EFI_D_INFO, "GenetPhyUpdateConfig: Link is down\n"));
        }
    }

    Genet->PhyLinkUp = LinkUp;

    return LinkUp ? EFI_SUCCESS : EFI_NOT_READY;
}

VOID
EFIAPI
GenetReset (
    IN GENET_PRIVATE_DATA * Genet
    )
{
    UINT32 Value;

    Value = GenetMmioRead (Genet, GENET_SYS_RBUF_FLUSH_CTRL);
    Value |= GENET_SYS_RBUF_FLUSH_RESET;
    GenetMmioWrite (Genet, GENET_SYS_RBUF_FLUSH_CTRL, Value);
    gBS->Stall (10);

    Value &= ~GENET_SYS_RBUF_FLUSH_RESET;
    GenetMmioWrite (Genet, GENET_SYS_RBUF_FLUSH_CTRL, Value);
    gBS->Stall (10);

    GenetMmioWrite (Genet, GENET_SYS_RBUF_FLUSH_CTRL, 0);
    gBS->Stall (10);

    GenetMmioWrite (Genet, GENET_UMAC_CMD, 0);
    GenetMmioWrite (Genet, GENET_UMAC_CMD, GENET_UMAC_CMD_LCL_LOOP_EN | GENET_UMAC_CMD_SW_RESET);
    gBS->Stall (10);
    GenetMmioWrite (Genet, GENET_UMAC_CMD, 0);

    GenetMmioWrite (Genet, GENET_UMAC_MIB_CTRL, GENET_UMAC_MIB_RESET_RUNT | GENET_UMAC_MIB_RESET_RX | GENET_UMAC_MIB_RESET_TX);
    GenetMmioWrite (Genet, GENET_UMAC_MIB_CTRL, 0);

    GenetMmioWrite (Genet, GENET_UMAC_MAX_FRAME_LEN, 1536);

    Value = GenetMmioRead (Genet, GENET_RBUF_CTRL);
    Value |= GENET_RBUF_ALIGN_2B;
    GenetMmioWrite (Genet, GENET_RBUF_CTRL, Value);

    GenetMmioWrite (Genet, GENET_RBUF_TBUF_SIZE_CTRL, 1);
}

VOID
EFIAPI
GenetSetMacAddress (
    IN GENET_PRIVATE_DATA * Genet,
    IN EFI_MAC_ADDRESS *    MacAddr
    )
{
    UINT32 Value;

    Value = MacAddr->Addr[3] |
            MacAddr->Addr[2] << 8 |
            MacAddr->Addr[1] << 16 |
            MacAddr->Addr[0] << 24;
    GenetMmioWrite (Genet, GENET_UMAC_MAC0, Value);
    Value = MacAddr->Addr[5] |
            MacAddr->Addr[4] << 8;
    GenetMmioWrite (Genet, GENET_UMAC_MAC1, Value);
}

VOID
EFIAPI
GenetSetPhyMode (
    IN GENET_PRIVATE_DATA * Genet,
    IN GENET_PHY_MODE       PhyMode
    )
{
    UINT32 Value;

    switch (PhyMode) {
        case GENET_PHY_MODE_RGMII:
        case GENET_PHY_MODE_RGMII_RXID:
            Value = GENET_SYS_PORT_MODE_EXT_GPHY;
            break;
        default:
            Value = 0;
            break;
    }
    GenetMmioWrite (Genet, GENET_SYS_PORT_CTRL, Value);
}

VOID
EFIAPI
GenetEnableTxRx (
    IN GENET_PRIVATE_DATA * Genet,
    IN BOOLEAN              Enable
    )
{
    UINT32 Value;

    Value = GenetMmioRead (Genet, GENET_UMAC_CMD);
    if (Enable) {
        Value |= GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN;
    } else {
        Value &= ~(GENET_UMAC_CMD_TXEN | GENET_UMAC_CMD_RXEN);
    }
    GenetMmioWrite (Genet, GENET_UMAC_CMD, Value);
}

VOID
EFIAPI
GenetSetPromisc (
    IN GENET_PRIVATE_DATA * Genet,
    IN BOOLEAN              Enable
    )
{
    UINT32 Value;

    Value = GenetMmioRead (Genet, GENET_UMAC_CMD);
    if (Enable) {
        Value |= GENET_UMAC_CMD_PROMISC;
    } else {
        Value &= ~GENET_UMAC_CMD_PROMISC;
    }
    GenetMmioWrite (Genet, GENET_UMAC_CMD, Value);
}