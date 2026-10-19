# Shopping list: MCU dev kits for the keybed and panel (draft)

Founder decision 2026-10-19: candidate family STM32G4, kit ST Nucleo-G474RE. Not final until Story 11.7. Prices and Brazilian sellers are NOT checked; search by the names below and compare final prices with tax and shipping.

| Item | Qty | Notes |
|---|---|---|
| ST Nucleo-G474RE (STM32G474RE, on-board ST-LINK) | 2 | One for the keybed, one for the panel. Confirm the listing says G474RE (not another Nucleo) |
| USB cable (Micro-B or Mini-B as on the board; check the board) | 2 | Powers and programs the board |
| Dupont jumper wires, female-female and male-female | 1 pack | For the first wiring tests |
| Breadboard | 1 | Optional, for the first tests |
| Multimeter with a continuity buzzer | 1 | Needed for the harness checklist |
| Connector or adapter to the Fatar 73SW harness | TBD | Connector type is not in the repo; get it from the keybed partner (Story 11.6) |
| 3.3 V USB-serial adapter (already being bought) | 1 | Used on the Orange Pi debug UART; can also read the MCU link |

Open: confirm the pin counts needed for 73 keys with 2 or 3 contacts each and decide the scan matrix (Story 3.3). The Nucleo's pin count may require a custom adapter board.
