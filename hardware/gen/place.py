"""Placement.  Coordinates in mm, origin top-left, x right, y down.  Rotation is KiCad's (CCW positive).

HDMI runs west to east along the top of the board: J1 -> D1/D2 -> U2 (GSV2001) -> D3/D4 -> J2.
U2 is rotated so its RX side faces west, TXA east and TXB south towards U3 (MS2130).
The connectors sit 4 mm apart in y so that every TMDS pair is a straight line into the chip.
"""
W, H = 78.0, 60.0

U2X, U2Y = 39.0, 19.5       # GSV2001
J1Y, J2Y = U2Y - 2.8, U2Y + 1.2
U3X, U3Y = 51.55, 39.05      # MS2130
U1X, U1Y = 26.5, 44.0       # ESP32-S31 module pad-array centre (antenna overhangs the south edge)


# Cut-out in the south edge under the module's antenna: Espressif wants the antenna beyond the board, or the board
# removed under and beside it.  The antenna area is the last 6 mm of the module (y >= U1Y + 10.37).
NOTCH = (15.6, U1Y + 10.4, 36.9, H)


def outline(B):
    x1, y1, x2, _ = NOTCH
    B.edge([(0, 0), (W, 0), (W, H), (x2, H), (x2, y1), (x1, y1), (x1, H), (0, H)])


def place_all(B):
    p = B.place
    for ref, xy in zip(("H1", "H2", "H3", "H4"), ((3.5, 3.5), (W - 3.5, 3.5), (3.5, H - 3.5), (W - 3.5, H - 3.5))):
        p(ref, *xy)

    # ---- connectors and the three big parts
    p("J1", 3.5, J1Y, -90)            # HDMI in, opening west
    p("J2", W - 3.5, J2Y, 90)         # HDMI out, opening east
    p("J3", 2.9, 47.0, -90)           # USB-C, opening west
    p("J4", W - 8.5, 48.5, 90)        # LED terminal, plug side east
    p("U2", U2X, U2Y, 90)
    p("U3", U3X, U3Y, 90)
    p("U1", U1X, U1Y, 180)
    p("J5", 31.0, 3.5, 90)            # debug header along the top edge

    # ---- TMDS ESD arrays, in line with the pairs
    p("D2", 12.6, J1Y + 0.5, 180)    # RX D0 + clock
    p("D1", 12.6, J1Y + 3.6, 180)    # RX D2 + D1
    p("D3", 65.4, J2Y - 3.5, 0)      # TX D2 + D1
    p("D4", 65.4, J2Y - 0.4, 0)      # TX D0 + clock

    # ---- HDMI in support (north-west)
    p("R13", 11.6, 10.3, 180)   # HPD series, connector side west
    p("C14", 11.6, 12.0, 0)
    p("R11", 15.0, 10.3, 0)
    p("R12", 15.0, 12.0, 0)
    for ref, x, y in (("TP11", 18.0, 10.0), ("TP10", 18.0, 12.3), ("TP12", 20.3, 10.0), ("TP13", 20.3, 12.3),
                      ("TP9", 26.8, 7.5), ("TP18", 61.4, 11.7)):
        p(ref, x, y)
    # CEC tap
    p("R18", 23.2, 8.0, 0)
    p("R20", 23.2, 9.7, 180)     # CEC end east, under R18's CEC end
    p("R19", 23.2, 12.6, 0)
    p("Q1", 26.8, 11.3, 0)

    # ---- GSV2001 control pulls and test points (north of U2)
    for i, ref in enumerate(("R25", "R24", "R26", "R27", "R23", "R22")):
        p(ref, 45.2 + 1.6 * i, 9.2, 90)
    # test pads in the same west-to-east order as U2 pins 57..54, so the fan-out does not cross
    for i, ref in enumerate(("TP22", "TP20", "TP19", "TP21")):     # AVMUTE, INT, RESETB, CSB
        p(ref, 29.1 + 2.3 * i, 7.5)
    p("TP23", 40.6, 10.7)                                          # AUD_D0, straight above its pin

    # ---- GSV2001 decoupling ring
    p("C25", 44.6, 13.2, 90)     # 1V2  pins 39/40
    p("C20", 41.8, 13.2, 90)     # 3V3  pin 41
    p("C26", 39.6, 13.2, 90)     # 1V2  pins 42/43
    p("C19", 46.0, 15.3, 0)      # 3V3  pin 38
    p("C24", 46.0, 16.3, 0)      # 1V2  pins 36/37
    p("C30", 47.0, 13.3, 0)      # 10uF 1V2
    p("C27", 32.0, 15.3, 180)    # 1V2  pins 58/59
    p("C18", 32.0, 16.3, 180)    # 3V3  pin 60
    p("C28", 29.9, 24.5, 0)    # 1V2  pin 70
    p("R21", 29.2, 22.9, 0)      # REXT 402R to 3V3
    p("C34", 46.2, 20.95, 0)     # TX_PVDD pin 24
    p("C35", 47.4, 26.6, 90)     # TX_PVDD 1uF
    p("FB2", 49.0, 26.6, 90)
    p("C23", 46.0, 26.3, 90)      # 1V2  pin 25
    p("C17", 48.0, 22.9, 0)      # 3V3  pin 26
    p("C22", 44.9, 26.3, 270)    # 1V2  pin 16
    p("C16", 33.9, 25.6, 180)    # 3V3  pin 1
    p("C29", 31.0, 26.6, 180)    # 10uF 3V3
    p("C32", 35.6, 26.3, 270)    # MPLL_PVDD pin 2
    p("C33", 30.4, 32.4, 0)      # MPLL_PVDD 1uF
    p("FB1", 33.6, 32.4, 0)
    p("Y1", 32.4, 29.6, 180)
    p("C36", 35.3, 29.6, 270)    # XTALI
    p("C37", 29.5, 30.0, 90)     # XTALO
    p("C21", 28.5, 26.4, 0)      # 1V2  pins 5/6
    p("C31", 27.4, 24.6, 0)     # 10uF 1V2 (RX side)
    p("R33", 51.2, 26.4, 90)     # TXB DDC pull-ups
    p("R34", 52.8, 26.4, 90)
    for ref, x, y in (("TP24", 43.2, 29.6), ("TP25", 45.4, 29.6), ("TP26", 47.6, 29.6), ("TP27", 49.8, 29.6)):
        p(ref, x, y)

    # ---- HDMI out support (south of the TX pairs)
    p("R17", 64.0, 9.0, 0)      # ARC link
    p("R14", 62.6, 23.5, 0)
    p("R15", 62.6, 25.1, 0)
    p("R16", 66.2, 25.6, 0)
    p("F2", 62.3, 28.5, 0)
    p("C15", 66.3, 28.6, 0)
    for ref, x, y in (("TP17", 54.7, 25.6), ("TP16", 57.0, 25.6), ("TP15", 59.3, 25.6), ("TP14", 58.3, 28.7)):
        p(ref, x, y)

    # ---- MS2130 ring
    p("C40", 45.4, 34.6, 180)     # AVDD33 pin 48
    p("C52", 48.25, 31.9, 90)     # AVDD25 pin 47
    p("R30", 49.55, 32.2, 90)     # RX REXT
    p("R32", 57.2, 32.0, 90)     # GPIO1 strap (DNP)
    p("C49", 51.1, 31.7, 90)     # 3V3 pin 41
    p("C61", 52.3, 31.7, 90)     # DVDD11 pin 39
    p("C48", 55.6, 31.7, 90)     # 3V3 pin 33
    p("FB4", 54.6, 48.0, 270)
    p("C62", 56.2, 48.0, 90)     # DVDD11 10uF
    p("D5", 52.6, 28.9, 0)
    p("R31", 55.7, 28.9, 0)
    p("C47", 57.7, 35.6, 0)      # 3V3 pin 31
    p("R35", 63.2, 36.05, 0)
    p("R36", 63.2, 37.65, 180)
    p("Y2", 60.5, 40.6, 90)
    p("C64", 59.65, 37.5, 90)    # XTOUT
    p("C63", 63.3, 41.7, 0)      # XTIN
    p("R28", 62.2, 43.55, 0)     # 1M
    # south of U3
    p("C45", 46.9, 45.4, 270)     # 3V3 pin 1
    p("C38", 43.9, 42.75, 180)     # AVDD33 pin 2
    p("C39", 51.3, 45.4, 270)     # AVDD33 pin 8
    p("R29", 50.0, 47.4, 270)     # USB REXT... placed after the pair leaves
    p("C53", 52.4, 45.4, 270)     # 1V1 pin 6
    p("C54", 53.5, 45.4, 270)     # 1V1 pin 10/12
    p("C46", 55.4, 45.4, 270)     # 3V3 pin 16
    p("C50", 57.7, 41.55, 0)     # AVDD25 pin 20
    p("C51", 57.7, 39.55, 0)     # AVDD25 pin 23
    p("C60", 57.7, 42.6, 0)     # DVDD11 pin 18
    # west of U3, between the TMDS stubs and the bus
    p("C41", 45.4, 36.5, 180)
    p("C42", 45.4, 38.55, 180)
    p("C43", 45.4, 40.55, 180)
    p("C56", 54.4, 50.5, 90)
    p("C57", 55.5, 50.5, 90)
    p("C58", 56.6, 50.5, 90)
    p("C44", 44.7, 32.2, 90)      # AVDD33 10uF
    p("FB3", 46.3, 32.2, 90)
    p("C55", 52.7, 50.5, 90)     # 1V1, over the feed from the buck
    p("C59", 52.7, 48.0, 90)      # 1V1 10uF
    p("U4", 63.2, 32.45, 0)
    p("C65", 65.8, 36.6, 0)

    # ---- 1.1 V buck and 2.5 V LDO (south-east, next to the MS2130)
    p("U7", 44.6, 53.6, 180)
    p("C10", 43.2, 50.6, 0)
    p("L3", 48.9, 53.6, 0)
    p("C11", 52.6, 53.6, 90)
    p("R8", 46.4, 56.9, 180)
    p("R9", 43.2, 56.9, 180)
    p("R10", 40.9, 54.4, 90)
    p("U8", 58.6, 54.3, 0)
    p("C12", 58.6, 57.3, 0)
    p("C13", 61.6, 54.3, 90)
    p("TP4", 58.9, 50.4)
    p("TP5", 61.6, 57.3)

    # ---- LED output
    p("U9", 63.4, 46.5, 90)
    p("C69", 63.2, 49.4, 0)
    p("R43", 66.1, 42.4, 90)
    p("D7", 65.9, 50.6, 90)
    p("R44", 65.4, 39.9, 0)
    p("TP28", 65.6, 53.7)

    # ---- power entry and the 1.2 V / 3.3 V bucks (west, between J1 and J3)
    p("U10", 11.6, 47.0, 0)
    p("R1", 10.6, 42.6, 0)
    p("R2", 10.6, 51.2, 0)
    p("F1", 11.0, 39.9, 0)
    p("C1", 11.4, 37.0, 0)
    p("C2", 14.7, 37.0, 0)
    p("U6", 4.4, 29.6, 180)
    p("C3", 4.6, 26.9, 0)
    p("L1", 8.7, 29.6, 0)
    p("C4", 12.3, 28.8, 90)
    p("C5", 14.4, 28.8, 90)
    p("R3", 4.2, 32.6, 0)
    p("R4", 7.4, 32.6, 0)
    p("U5", 18.6, 27.6, 180)
    p("C6", 18.4, 24.9, 0)
    p("L2", 22.9, 27.6, 0)
    p("C7", 26.5, 27.6 + 0.3, 90)
    p("C8", 26.5, 31.9, 0)
    p("R5", 18.4, 30.9, 0)
    p("R6", 18.4, 32.5, 0)
    p("R7", 21.6, 30.9, 0)
    p("C9", 21.6, 32.5, 0)
    p("TP1", 14.4, 42.9)
    p("TP2", 24.6, 24.4)
    p("TP3", 12.0, 25.3)
    p("TP6", 8.6, 36.4)
    p("TP7", 69.6, 9.4)
    p("TP8", 40.6, 57.6)

    # ---- ESP32 support
    p("C66", 38.3, 52.0, 90)
    p("C67", 38.3, 55.4, 90)
    p("R37", 38.3, 48.6, 90)
    p("C68", 38.3, 45.4, 90)     # moved clear of the USB pair below
    p("SW1", 12.6, 54.2, 0)
    p("SW2", 12.6, 57.6, 0)
    p("R38", 14.6, 34.6, 0)
    p("R39", 38.2, 29.4, 90)
    p("R40", 39.8, 29.4, 90)
    p("D6", 56.4, 8.4, 0)
    p("R41", 59.6, 8.4, 0)
    p("R42", 15.4, 49.4, 90)

    # ---- later additions
    for i, ref in enumerate(("R45", "R46", "R47", "R48")):      # flash access from the ESP32, next to the flash
        p(ref, 68.2 + 1.6 * i, 32.0, 90)
    # ---- additions from the pre-order review
    p("R52", 59.8, 45.3, 0)        # TXB hot-plug pull-down
    p("R53", 42.8, 32.25, 90)      # TXB utility pull-down
    p("R54", 68.2, 53.7, 0)        # LED data pull-down
    p("R55", 11.6, 8.5, 180)       # HDMI-in hot-plug pull-up option (not fitted)
    p("D8", 15.0, 32.4, 0)         # empties the 3V3 enable delay when 5 V drops
    p("R56", 60.75, 50.9, 90)      # 1V1 bleed
    p("R57", 38.6, 6.9, 90)        # console RX pull-up
    p("R58", 38.05, 40.3, 0)       # ESP32 IO7 -> MS2130 GPIO0 (plug detect), beside the module pad
    p("TP29", 49.5, 58.0)          # MS_PWR_EN
    p("C70", 64.0, 10.7, 0)                                      # eARC coupling capacitor (not fitted)
    p("R49", 23.95, 32.4, 270)                                   # GPIO37 strap pull-up
    p("R50", 26.1, 22.45, 0)                                     # second half of the GSV2001's 400 ohm REXT
    p("R51", 40.9, 43.6, 0)                                     # 100R in series with R30 (MS2130 REXT)
