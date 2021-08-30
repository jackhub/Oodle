// Copyright Epic Games, Inc. All Rights Reserved.
// This source file is licensed solely to users who have
// accepted a valid Unreal Engine license agreement 
// (see e.g., https://www.unrealengine.com/eula), and use
// of this source file is governed by such agreement.


RR_NAMESPACE_START

#if 1

static const int COTable5_4C_size = 512;
static const U8 COTable5_4C[] = 
{
  0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x01,0x00,0x01,0x00,0x01,0x00,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x02,0x02,0x01,0x02,0x01,0x02,0x01,
0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x03,0x02,0x03,0x03,0x02,0x03,0x02,0x03,0x02,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x04,0x03,0x04,0x03,0x04,0x03,0x05,
0x04,0x03,0x04,0x03,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x05,0x04,0x05,0x05,0x04,0x05,0x04,0x05,0x04,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x06,0x05,0x06,
0x06,0x05,0x06,0x05,0x06,0x05,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x07,0x06,0x07,0x07,0x06,0x07,0x06,0x07,0x06,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x08,
0x07,0x08,0x07,0x08,0x07,0x09,0x08,0x07,0x08,0x07,0x07,0x0A,0x08,0x08,0x08,0x08,0x08,0x09,0x08,0x09,0x09,0x08,0x09,0x08,0x09,0x08,0x09,0x09,0x09,0x09,
0x09,0x09,0x09,0x0A,0x09,0x0A,0x0A,0x09,0x0A,0x09,0x0A,0x09,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0B,0x0A,0x0B,0x0B,0x0A,0x0B,0x0A,0x0B,0x0A,0x0B,0x0B,
0x0B,0x0B,0x0B,0x0B,0x0B,0x0C,0x0B,0x0C,0x0B,0x0C,0x0B,0x0D,0x0C,0x0B,0x0C,0x0B,0x0B,0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x0D,0x0C,0x0D,0x0D,0x0C,0x0D,0x0C,
0x0D,0x0C,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0E,0x0D,0x0E,0x0E,0x0D,0x0E,0x0D,0x0E,0x0D,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0F,0x0E,0x0F,0x0F,0x0E,
0x0F,0x0E,0x0F,0x0E,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x10,0x0F,0x10,0x0F,0x10,0x0F,0x11,0x10,0x0F,0x10,0x0F,0x0F,0x12,0x10,0x10,0x10,0x10,0x10,0x11,
0x10,0x11,0x11,0x10,0x11,0x10,0x11,0x10,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x12,0x11,0x12,0x12,0x11,0x12,0x11,0x12,0x11,0x12,0x12,0x12,0x12,0x12,0x12,
0x12,0x13,0x12,0x13,0x13,0x12,0x13,0x12,0x13,0x12,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x14,0x13,0x14,0x13,0x14,0x13,0x15,0x14,0x13,0x14,0x13,0x13,0x16,
0x14,0x14,0x14,0x14,0x14,0x15,0x14,0x15,0x15,0x14,0x15,0x14,0x15,0x14,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x16,0x15,0x16,0x16,0x15,0x16,0x15,0x16,0x15,
0x16,0x16,0x16,0x16,0x16,0x16,0x16,0x17,0x16,0x17,0x17,0x16,0x17,0x16,0x17,0x16,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x18,0x17,0x18,0x17,0x18,0x17,0x19,
0x18,0x17,0x18,0x17,0x17,0x1A,0x18,0x18,0x18,0x18,0x18,0x19,0x18,0x19,0x19,0x18,0x19,0x18,0x19,0x18,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x1A,0x19,0x1A,
0x1A,0x19,0x1A,0x19,0x1A,0x19,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1B,0x1A,0x1B,0x1B,0x1A,0x1B,0x1A,0x1B,0x1A,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1C,
0x1B,0x1C,0x1B,0x1C,0x1B,0x1D,0x1C,0x1B,0x1C,0x1B,0x1B,0x1E,0x1C,0x1C,0x1C,0x1C,0x1C,0x1D,0x1C,0x1D,0x1D,0x1C,0x1D,0x1C,0x1D,0x1C,0x1D,0x1D,0x1D,0x1D,
0x1D,0x1D,0x1D,0x1E,0x1D,0x1E,0x1E,0x1D,0x1E,0x1D,0x1E,0x1D,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1F,0x1E,0x1F,0x1F,0x1E,0x1F,0x1E,0x1F,0x1E,0x1F,0x1F,
0x1F,0x1F
};


static const int COTable6_4C_size = 512;
static const U8 COTable6_4C[] = 
{
  0x00,0x00,0x00,0x01,0x01,0x00,0x01,0x01,0x01,0x01,0x01,0x02,0x02,0x01,0x02,0x02,0x02,0x02,0x02,0x03,0x03,0x02,0x03,0x03,0x03,0x03,0x03,0x04,0x04,0x03,
0x04,0x04,0x04,0x04,0x04,0x05,0x05,0x04,0x05,0x05,0x05,0x05,0x05,0x06,0x06,0x05,0x06,0x06,0x06,0x06,0x06,0x07,0x07,0x06,0x07,0x07,0x07,0x07,0x07,0x08,
0x08,0x07,0x08,0x08,0x08,0x08,0x08,0x09,0x09,0x08,0x09,0x09,0x09,0x09,0x09,0x0A,0x0A,0x09,0x0A,0x0A,0x0A,0x0A,0x0A,0x0B,0x0B,0x0A,0x0B,0x0B,0x0B,0x0B,
0x0B,0x0C,0x0C,0x0B,0x0C,0x0C,0x0C,0x0C,0x0C,0x0D,0x0D,0x0C,0x0D,0x0D,0x0D,0x0D,0x0D,0x0E,0x0E,0x0D,0x0E,0x0E,0x0E,0x0E,0x0E,0x0F,0x0F,0x0E,0x0E,0x10,
0x0F,0x0F,0x0F,0x10,0x10,0x0E,0x10,0x0F,0x10,0x10,0x10,0x10,0x10,0x11,0x11,0x10,0x11,0x11,0x11,0x11,0x11,0x12,0x12,0x11,0x12,0x12,0x12,0x12,0x12,0x13,
0x13,0x12,0x13,0x13,0x13,0x13,0x13,0x14,0x14,0x13,0x14,0x14,0x14,0x14,0x14,0x15,0x15,0x14,0x15,0x15,0x15,0x15,0x15,0x16,0x16,0x15,0x16,0x16,0x16,0x16,
0x16,0x17,0x17,0x16,0x17,0x17,0x17,0x17,0x17,0x18,0x18,0x17,0x18,0x18,0x18,0x18,0x18,0x19,0x19,0x18,0x19,0x19,0x19,0x19,0x19,0x1A,0x1A,0x19,0x1A,0x1A,
0x1A,0x1A,0x1A,0x1B,0x1B,0x1A,0x1B,0x1B,0x1B,0x1B,0x1B,0x1C,0x1C,0x1B,0x1C,0x1C,0x1C,0x1C,0x1C,0x1D,0x1D,0x1C,0x1D,0x1D,0x1D,0x1D,0x1D,0x1E,0x1E,0x1D,
0x1E,0x1E,0x1E,0x1E,0x1E,0x1F,0x1F,0x1E,0x1E,0x20,0x1F,0x1F,0x1F,0x20,0x20,0x1E,0x20,0x1F,0x1F,0x22,0x20,0x20,0x20,0x21,0x21,0x20,0x21,0x21,0x21,0x21,
0x21,0x22,0x22,0x21,0x22,0x22,0x22,0x22,0x22,0x23,0x23,0x22,0x23,0x23,0x23,0x23,0x23,0x24,0x24,0x23,0x24,0x24,0x24,0x24,0x24,0x25,0x25,0x24,0x25,0x25,
0x25,0x25,0x25,0x26,0x26,0x25,0x26,0x26,0x26,0x26,0x26,0x27,0x27,0x26,0x27,0x27,0x27,0x27,0x27,0x28,0x28,0x27,0x28,0x28,0x28,0x28,0x28,0x29,0x29,0x28,
0x29,0x29,0x29,0x29,0x29,0x2A,0x2A,0x29,0x2A,0x2A,0x2A,0x2A,0x2A,0x2B,0x2B,0x2A,0x2B,0x2B,0x2B,0x2B,0x2B,0x2C,0x2C,0x2B,0x2C,0x2C,0x2C,0x2C,0x2C,0x2D,
0x2D,0x2C,0x2D,0x2D,0x2D,0x2D,0x2D,0x2E,0x2E,0x2D,0x2E,0x2E,0x2E,0x2E,0x2E,0x2F,0x2F,0x2E,0x2E,0x30,0x2F,0x2F,0x2F,0x30,0x2F,0x30,0x30,0x2F,0x2F,0x32,
0x30,0x30,0x30,0x31,0x31,0x30,0x31,0x31,0x31,0x31,0x31,0x32,0x32,0x31,0x32,0x32,0x32,0x32,0x32,0x33,0x33,0x32,0x33,0x33,0x33,0x33,0x33,0x34,0x34,0x33,
0x34,0x34,0x34,0x34,0x34,0x35,0x35,0x34,0x35,0x35,0x35,0x35,0x35,0x36,0x36,0x35,0x36,0x36,0x36,0x36,0x36,0x37,0x37,0x36,0x37,0x37,0x37,0x37,0x37,0x38,
0x38,0x37,0x38,0x38,0x38,0x38,0x38,0x39,0x39,0x38,0x39,0x39,0x39,0x39,0x39,0x3A,0x3A,0x39,0x3A,0x3A,0x3A,0x3A,0x3A,0x3B,0x3B,0x3A,0x3B,0x3B,0x3B,0x3B,
0x3B,0x3C,0x3C,0x3B,0x3C,0x3C,0x3C,0x3C,0x3C,0x3D,0x3D,0x3C,0x3D,0x3D,0x3D,0x3D,0x3D,0x3E,0x3E,0x3D,0x3E,0x3E,0x3E,0x3E,0x3E,0x3F,0x3F,0x3E,0x3F,0x3F,
0x3F,0x3F
};


static const int COTable5_3C_size = 512;
static const U8 COTable5_3C[] = 
{
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x01,0x02,0x01,0x02,0x02,0x02,
0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x03,0x02,0x03,0x02,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x04,0x03,0x04,0x03,0x04,
0x03,0x04,0x03,0x05,0x03,0x05,0x04,0x04,0x04,0x04,0x03,0x06,0x03,0x06,0x04,0x05,0x04,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x06,
0x05,0x06,0x05,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x07,0x06,0x07,0x06,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
0x07,0x08,0x07,0x08,0x07,0x08,0x07,0x08,0x07,0x09,0x07,0x09,0x08,0x08,0x08,0x08,0x07,0x0A,0x07,0x0A,0x08,0x09,0x08,0x09,0x09,0x09,0x09,0x09,0x09,0x09,
0x09,0x09,0x09,0x09,0x09,0x0A,0x09,0x0A,0x09,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0B,0x0A,0x0B,0x0A,0x0B,0x0B,0x0B,0x0B,0x0B,
0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0B,0x0C,0x0B,0x0C,0x0B,0x0C,0x0B,0x0C,0x0B,0x0D,0x0B,0x0D,0x0C,0x0C,0x0C,0x0C,0x0B,0x0E,0x0B,0x0E,0x0C,0x0D,0x0C,0x0D,
0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0E,0x0D,0x0E,0x0D,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0F,0x0E,0x0F,
0x0E,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x0F,0x10,0x0F,0x10,0x0F,0x10,0x0F,0x10,0x0F,0x11,0x0F,0x11,0x10,0x10,0x10,0x10,0x0F,0x12,
0x0F,0x12,0x10,0x11,0x10,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x12,0x11,0x12,0x11,0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12,
0x12,0x12,0x12,0x13,0x12,0x13,0x12,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x14,0x13,0x14,0x13,0x14,0x13,0x14,0x13,0x15,0x13,0x15,
0x14,0x14,0x14,0x14,0x13,0x16,0x14,0x15,0x14,0x15,0x14,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x16,0x15,0x16,0x15,0x16,0x16,0x16,
0x16,0x16,0x16,0x16,0x16,0x16,0x16,0x16,0x16,0x17,0x16,0x17,0x16,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x18,0x17,0x18,0x17,0x18,
0x17,0x18,0x17,0x19,0x17,0x19,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x19,0x18,0x19,0x18,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x1A,
0x19,0x1A,0x19,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1A,0x1B,0x1A,0x1B,0x1A,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,
0x1B,0x1C,0x1B,0x1C,0x1B,0x1C,0x1B,0x1C,0x1B,0x1D,0x1B,0x1D,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1D,0x1C,0x1D,0x1C,0x1D,0x1D,0x1D,0x1D,0x1D,0x1D,0x1D,
0x1D,0x1D,0x1D,0x1D,0x1D,0x1E,0x1D,0x1E,0x1D,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1F,0x1E,0x1F,0x1E,0x1F,0x1F,0x1F,0x1F,0x1F,
0x1F,0x1F
};


static const int COTable6_3C_size = 512;
static const U8 COTable6_3C[] = 
{
  0x00,0x00,0x00,0x00,0x00,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x04,
0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x05,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x06,0x07,0x07,0x07,0x07,0x07,0x07,0x07,
0x07,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x09,0x09,0x09,0x09,0x09,0x09,0x09,0x09,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0A,0x0B,0x0B,0x0B,0x0B,0x0B,
0x0B,0x0B,0x0B,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0D,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0E,0x0F,0x0F,0x0F,
0x0F,0x0F,0x0F,0x0F,0x0F,0x10,0x0F,0x10,0x0F,0x11,0x10,0x10,0x0F,0x12,0x10,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x12,0x12,0x12,0x12,0x12,0x12,0x12,
0x12,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x13,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x14,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x15,0x16,0x16,0x16,0x16,0x16,
0x16,0x16,0x16,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x17,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x19,0x1A,0x1A,0x1A,
0x1A,0x1A,0x1A,0x1A,0x1A,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1C,0x1D,0x1D,0x1D,0x1D,0x1D,0x1D,0x1D,0x1D,0x1E,
0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1E,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x20,0x1F,0x20,0x1F,0x21,0x20,0x20,0x1F,0x22,0x20,0x21,0x21,0x21,0x21,0x21,
0x21,0x21,0x21,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x22,0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x23,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x24,0x25,0x25,0x25,
0x25,0x25,0x25,0x25,0x25,0x26,0x26,0x26,0x26,0x26,0x26,0x26,0x26,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x27,0x28,0x28,0x28,0x28,0x28,0x28,0x28,0x28,0x29,
0x29,0x29,0x29,0x29,0x29,0x29,0x29,0x2A,0x2A,0x2A,0x2A,0x2A,0x2A,0x2A,0x2A,0x2B,0x2B,0x2B,0x2B,0x2B,0x2B,0x2B,0x2B,0x2C,0x2C,0x2C,0x2C,0x2C,0x2C,0x2C,
0x2C,0x2D,0x2D,0x2D,0x2D,0x2D,0x2D,0x2D,0x2D,0x2E,0x2E,0x2E,0x2E,0x2E,0x2E,0x2E,0x2E,0x2F,0x2F,0x2F,0x2F,0x2F,0x2F,0x2F,0x2F,0x30,0x2F,0x30,0x2F,0x31,
0x30,0x30,0x30,0x30,0x30,0x31,0x31,0x31,0x31,0x31,0x31,0x31,0x31,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x32,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x33,0x34,
0x34,0x34,0x34,0x34,0x34,0x34,0x34,0x35,0x35,0x35,0x35,0x35,0x35,0x35,0x35,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x37,0x37,0x37,0x37,0x37,0x37,0x37,
0x37,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x38,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x39,0x3A,0x3A,0x3A,0x3A,0x3A,0x3A,0x3A,0x3A,0x3B,0x3B,0x3B,0x3B,0x3B,
0x3B,0x3B,0x3B,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3C,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3D,0x3E,0x3E,0x3E,0x3E,0x3E,0x3E,0x3E,0x3E,0x3F,0x3F,0x3F,
0x3F,0x3F
};

#endif

RR_NAMESPACE_END

