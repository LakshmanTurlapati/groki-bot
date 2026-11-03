// SPDX-FileCopyrightText: 2026 Joseph Zhou
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdint.h>

int SCCB_Init(int pin_sda, int pin_scl);
int SCCB_Use_Port(int port);
int SCCB_Deinit(void);
int SCCB_Probe(uint8_t address);
uint8_t SCCB_Read(uint8_t address, uint8_t reg);
int SCCB_Write(uint8_t address, uint8_t reg, uint8_t value);
uint8_t SCCB_Read16(uint8_t address, uint16_t reg);
int SCCB_Write16(uint8_t address, uint16_t reg, uint8_t value);
uint16_t SCCB_Read_Addr16_Val16(uint8_t address, uint16_t reg);
int SCCB_Write_Addr16_Val16(uint8_t address, uint16_t reg, uint16_t value);
