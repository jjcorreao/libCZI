//******************************************************************************
// 
// libCZI is a reader for the CZI fileformat written in C++
// Copyright (C) 2017  Zeiss Microscopy GmbH
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
// 
// To obtain a commercial version please contact Zeiss Microscopy GmbH.
// 
//******************************************************************************

#include "stdafx.h"

#include "MultiChannelCompositor.h"
#include "libCZI_Utilities.h"
#include <cmath>
#include "Site.h"

using namespace libCZI;
using namespace std;

//----------------------------------------------------------------------------------------------------

class CMultiChannelCompositor2
{
private:
	static bool IsUsingLut(const Compositors::ChannelInfo* channelInfo)
	{
		return channelInfo->lookUpTableElementCount == 0 ? false : true;
	}

	static bool IsBlackWhitePointUsed(const Compositors::ChannelInfo* channelInfo)
	{
		if (channelInfo->blackPoint <= 0 && channelInfo->whitePoint >= 1)
			return false;

		return true;
	}

	static uint8_t toByte(float f)
	{
		return (uint8_t)(f + .5f);
	}

	static int toInt(float f)
	{
		return (int)(f + .5f);
	}

	struct bgr8 { uint8_t b; uint8_t g; uint8_t r; };

	struct CGray8BytesPerPel
	{
		static const uint8_t bytesPerPel = 1;
	};

	struct CGray16BytesPerPel
	{
		static const uint8_t bytesPerPel = 2;
	};


	struct CBgr24BytesPerPel
	{
		static const uint8_t bytesPerPel = 3;
	};

	struct CBgr48BytesPerPel
	{
		static const uint8_t bytesPerPel = 6;
	};

	struct CGetGray8 : CGray8BytesPerPel
	{
		bgr8 operator()(const uint8_t* p) const
		{
			return bgr8{ *(p),*(p) ,*(p) };
		}
	};

	struct CGetGray16 : CGray16BytesPerPel
	{
		bgr8 operator()(const uint8_t* p) const
		{
			uint8_t v = (uint8_t)((*((const uint16_t*)p)) >> 8);
			return bgr8{ v,v,v };
		}
	};

	struct CGetBgr24 : CBgr24BytesPerPel
	{
		bgr8 operator()(const uint8_t* p) const
		{
			return bgr8{ *(p),*(p + 1),*(p + 2) };
		}
	};

	struct CGetBgr48 : CBgr48BytesPerPel
	{
		bgr8 operator()(const uint8_t* p) const
		{
			const uint16_t* pUs = (const uint16_t*)p;
			return bgr8{ (uint8_t)((*pUs) >> 8),(uint8_t)((*(pUs + 1)) >> 8),(uint8_t)((*(pUs + 2)) >> 8) };
		}
	};

	struct CGetTintedBase
	{
		Rgb8Color tintingColor;
		explicit CGetTintedBase(Rgb8Color tintingColor) :tintingColor(tintingColor) {};
	};

	struct CGetTintedGray8 : CGetTintedBase, CGray8BytesPerPel
	{
		explicit CGetTintedGray8(Rgb8Color tintingColor) :CGetTintedBase(tintingColor) {}
		bgr8 operator()(const uint8_t* p) const
		{
			float f = *p;
			f /= 255;
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetTintedGray16 : CGetTintedBase, CGray16BytesPerPel
	{
		explicit CGetTintedGray16(Rgb8Color tintingColor) : CGetTintedBase(tintingColor) {}
		bgr8 operator()(const uint8_t* p) const
		{
			float f = *((const uint16_t*)p);
			f /= (256 * 256 - 1);
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetTintedBgr24 : CGetTintedBase, CBgr24BytesPerPel
	{
		explicit CGetTintedBgr24(Rgb8Color tintingColor) :CGetTintedBase(tintingColor) {}
		bgr8 operator()(const uint8_t* p) const
		{
			float f = (float)(((int)p[0]) + p[1] + p[2]);
			f /= (3 * 255);
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetTintedBgr48 : CGetTintedBase, CBgr48BytesPerPel
	{
		explicit CGetTintedBgr48(Rgb8Color tintingColor) :CGetTintedBase(tintingColor) {}
		bgr8 operator()(const uint8_t* p) const
		{
			const uint16_t* puv = (const uint16_t*)p;
			float f = (float)(((int)(puv[0])) + puv[1] + puv[2]);
			f /= (3 * (256 * 256 - 1));
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	template <typename tStore, typename tGetRgb>
	static void CopyTinting(tStore store, tGetRgb& op, uint32_t w, uint32_t h, uint8_t* ptrDst, int strideDst, const uint8_t* ptrSrc, int strideSrc)
	{
		for (uint32_t y = 0; y < h; ++y)
		{
			const uint8_t* pSrc = ptrSrc + y * ((ptrdiff_t)strideSrc);
			uint8_t* pDst = ptrDst + y * ((ptrdiff_t)strideDst);
			for (uint32_t x = 0; x < w; ++x)
			{
				bgr8 bgr = op(pSrc);
				pSrc += tGetRgb::bytesPerPel;
				store(pDst + 3 * x, bgr);
			}
		}
	}

	template <typename tStore>
	static void DoTinting(tStore store, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst,
		libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		ScopedBitmapLockerP lckSrc{ src };
		if (chInfo->enableTinting)
		{
			switch (src->GetPixelType())
			{
			case PixelType::Gray8:
			{
				CGetTintedGray8 t{ chInfo->tinting.color };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Gray16:
			{
				CGetTintedGray16 t{ chInfo->tinting.color };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr24:
			{
				CGetTintedBgr24 t{ chInfo->tinting.color };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr48:
			{
				CGetTintedBgr48 t{ chInfo->tinting.color };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			default:
				throw std::runtime_error("Not implemented for this pixeltype.");
			}
		}
		else
		{
			switch (src->GetPixelType())
			{
			case PixelType::Gray8:
			{
				CGetGray8 t;
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Gray16:
			{
				CGetGray16 t;
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr24:
			{
				CGetBgr24 t;
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr48:
			{
				CGetBgr48 t;
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			default:
				throw std::runtime_error("Not implemented for this pixeltype.");
			}
		}
	}

	struct CAdd
	{
		void operator ()(uint8_t* ptrDst, const bgr8& val)
		{
			int p = *(ptrDst + 0);
			*(ptrDst + 0) = (uint8_t)(std::min)(p + val.b, 0xff);
			p = *(ptrDst + 1);
			*(ptrDst + 1) = (uint8_t)(std::min)(p + val.g, 0xff);
			p = *(ptrDst + 2);
			*(ptrDst + 2) = (uint8_t)(std::min)(p + val.r, 0xff);
		}
	};

	struct CStore
	{
		void operator ()(uint8_t* ptrDst, const bgr8& val)
		{
			*(ptrDst + 0) = val.b;
			*(ptrDst + 1) = val.g;
			*(ptrDst + 2) = val.r;
		}
	};

	struct CStoreWithWeightBase
	{
		float weight;
		explicit CStoreWithWeightBase(float w) :weight(w) {}
	};

	struct CAddWithWeight : protected CStoreWithWeightBase
	{
		explicit CAddWithWeight(float weight) : CStoreWithWeightBase(weight) {}
		void operator ()(uint8_t* ptrDst, const bgr8& val)
		{
			int p = *(ptrDst + 0);
			*(ptrDst + 0) = (uint8_t)(std::min)(p + toInt(val.b*this->weight), 0xff);
			p = *(ptrDst + 1);
			*(ptrDst + 1) = (uint8_t)(std::min)(p + toInt(val.g*this->weight), 0xff);
			p = *(ptrDst + 2);
			*(ptrDst + 2) = (uint8_t)(std::min)(p + toInt(val.r*this->weight), 0xff);
		}
	};

	struct CStoreWithWeight : protected CStoreWithWeightBase
	{
		explicit CStoreWithWeight(float weight) : CStoreWithWeightBase(weight) {}
		void operator ()(uint8_t* ptrDst, const bgr8& val)
		{
			*(ptrDst + 0) = (uint8_t)(std::min)(toInt(val.b*this->weight), 0xff);
			*(ptrDst + 1) = (uint8_t)(std::min)(toInt(val.g*this->weight), 0xff);
			*(ptrDst + 2) = (uint8_t)(std::min)(toInt(val.r*this->weight), 0xff);
		}
	};

	static void DoTintingCopy(libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CStore store;
		DoTinting<CStore>(store, dest, lckDst, src, chInfo);
	}

	static void DoTintingAdd(libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CAdd store;
		DoTinting<CAdd>(store, dest, lckDst, src, chInfo);
	}

	static void DoTintingCopy(float weight, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CStoreWithWeight store(weight);
		DoTinting<CStoreWithWeight>(store, dest, lckDst, src, chInfo);
	}

	static void DoTintingAdd(float weight, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CAddWithWeight store(weight);
		DoTinting<CAddWithWeight >(store, dest, lckDst, src, chInfo);
	}

	template <typename tValue, int maxValue>
	struct CGetBlackWhitePtBase
	{
		tValue blackPt, whitePt;

		bool CheckBlackWhite(tValue& v, uint8_t& pixelValue) const
		{
			if (v <= this->blackPt)
			{
				pixelValue = 0;
				return true;
			}
			if (v >= this->whitePt)
			{
				pixelValue = 0xff;
				return true;
			}

			return false;
		}

		bool CheckBlackWhiteFloat(tValue v, float& floatValue) const
		{
			if (v <= this->blackPt)
			{
				floatValue = 0;
				return true;
			}
			if (v >= this->whitePt)
			{
				floatValue = 1;
				return true;
			}

			return false;
		}

		CGetBlackWhitePtBase(float blackPt, float whitePt)
		{
			this->blackPt = (tValue)std::ceil(blackPt*maxValue);
			this->whitePt = (tValue)std::floor(whitePt*maxValue);
		};
	};

	struct CGetBlackWhitePtGray8 : CGetBlackWhitePtBase<std::uint8_t, 255>, CGray8BytesPerPel
	{
		CGetBlackWhitePtGray8(float blackPt, float whitePt) : CGetBlackWhitePtBase(blackPt, whitePt) {}
		bgr8 operator()(const uint8_t* p) const
		{
			uint8_t pv = this->GetPixelValue(p);
			return bgr8{ pv,pv,pv };
		}

		uint8_t GetPixelValue(const uint8_t* p) const
		{
			uint8_t pv = *p;
			uint8_t retPixelValue;
			if (this->CheckBlackWhite(pv, retPixelValue))
			{
				return retPixelValue;
			}

			float f = (pv - this->blackPt) / float(this->whitePt - this->blackPt);
			retPixelValue = (uint8_t)(f * 255 + .5);
			return retPixelValue;
		}

		float GetPixelValueFloat(const uint8_t* p) const
		{
			uint8_t pv = *p;
			float f;
			if (this->CheckBlackWhiteFloat(pv, f))
			{
				return f;
			}

			f = (pv - this->blackPt) / float(this->whitePt - this->blackPt);
			return f;
		}

		float GetPixelValueFloat(uint8_t pv) const
		{
			float f;
			if (this->CheckBlackWhiteFloat(pv, f))
			{
				return f;
			}

			f = (pv - this->blackPt) / float(this->whitePt - this->blackPt);
			return f;
		}
	};

	struct CGetTintedBlackWhitePtBase
	{
		Rgb8Color tintingColor;
		explicit CGetTintedBlackWhitePtBase(Rgb8Color tintingColor) :tintingColor(tintingColor) {}
	};

	struct CGetBlackWhitePtTintingGray8 :public CGetBlackWhitePtGray8, CGetTintedBlackWhitePtBase
	{
		explicit CGetBlackWhitePtTintingGray8(Rgb8Color tintingColor, float blackPt, float whitePt)
			: CGetBlackWhitePtGray8(blackPt, whitePt), CGetTintedBlackWhitePtBase(tintingColor)
		{}

		bgr8 operator()(const uint8_t* p) const
		{
			float f = this->GetPixelValueFloat(p);
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetBlackWhitePtGray16 :CGetBlackWhitePtBase<std::uint16_t, 256 * 256 - 1>, CGray16BytesPerPel
	{
		CGetBlackWhitePtGray16(float blackPt, float whitePt) :CGetBlackWhitePtBase(blackPt, whitePt) {}
		bgr8 operator()(const uint8_t* p) const
		{
			uint8_t pv = this->GetPixelValue(p);
			return bgr8{ pv,pv,pv };
		}

		uint8_t GetPixelValue(const uint8_t* p) const
		{
			uint16_t pv = *((const uint16_t*)p);
			uint8_t retPixelValue;
			if (this->CheckBlackWhite(pv, retPixelValue))
			{
				return retPixelValue;
			}

			float f = (pv - this->blackPt) / float(this->whitePt - this->blackPt);
			retPixelValue = (uint8_t)(f * 255 + .5);
			return retPixelValue;
		}

		float GetPixelValueFloat(const uint8_t* p) const
		{
			uint16_t pv = *((const uint16_t*)p);
			float f;
			if (this->CheckBlackWhiteFloat(pv, f))
			{
				return f;
			}

			f = (pv - this->blackPt) / float(this->whitePt - this->blackPt);
			return f;
		}

		float GetPixelValueFloat(uint16_t pv) const
		{
			float f;
			if (this->CheckBlackWhiteFloat(pv, f))
			{
				return f;
			}

			f = (pv - this->blackPt) / float(this->whitePt - this->blackPt);
			return f;
		}
	};

	struct CGetBlackWhitePtTintingGray16 : public CGetBlackWhitePtGray16, CGetTintedBlackWhitePtBase
	{
		CGetBlackWhitePtTintingGray16(Rgb8Color tintingColor, float blackPt, float whitePt)
			: CGetBlackWhitePtGray16(blackPt, whitePt), CGetTintedBlackWhitePtBase(tintingColor)
		{}

		bgr8 operator()(const uint8_t* p) const
		{
			float f = this->GetPixelValueFloat(p);
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetBlackWhitePtBgr24 : public CBgr24BytesPerPel
	{
	protected:
		CGetBlackWhitePtGray8 getBlkWhtGray8;
	public:
		CGetBlackWhitePtBgr24(float blackPt, float whitePt) :getBlkWhtGray8(blackPt, whitePt) {}
		bgr8 operator()(const uint8_t* p) const
		{
			uint8_t b = this->getBlkWhtGray8.GetPixelValue(p);
			uint8_t g = this->getBlkWhtGray8.GetPixelValue(p + 1);
			uint8_t r = this->getBlkWhtGray8.GetPixelValue(p + 2);
			return bgr8{ b,g,r };
		}
	};

	struct CGetBlackWhitePtTintingBgr24 : public CGetBlackWhitePtBgr24, CGetTintedBlackWhitePtBase
	{
		CGetBlackWhitePtTintingBgr24(Rgb8Color tintingColor, float blackPt, float whitePt)
			: CGetBlackWhitePtBgr24(blackPt, whitePt), CGetTintedBlackWhitePtBase(tintingColor)
		{}

		bgr8 operator()(const uint8_t* p) const
		{
			uint8_t pv = (uint8_t)((((int)(*p)) + *(p + 1) + *(p + 2) + 1) / 3);
			float f = this->getBlkWhtGray8.GetPixelValueFloat(pv);
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetBlackWhitePtBgr48 : public CBgr48BytesPerPel
	{
	protected:
		CGetBlackWhitePtGray16 getBlkWhtGray16;
	public:
		CGetBlackWhitePtBgr48(float blackPt, float whitePt) : getBlkWhtGray16(blackPt, whitePt) {}
		bgr8 operator()(const uint8_t* p) const
		{
			uint8_t b = this->getBlkWhtGray16.GetPixelValue(p);
			uint8_t g = this->getBlkWhtGray16.GetPixelValue(p + 2);
			uint8_t r = this->getBlkWhtGray16.GetPixelValue(p + 4);
			return bgr8{ b,g,r };
		}
	};

	struct CGetBlackWhitePtTintingBgr48 :public CGetBlackWhitePtBgr48, CGetTintedBlackWhitePtBase
	{
		CGetBlackWhitePtTintingBgr48(Rgb8Color tintingColor, float blackPt, float whitePt)
			: CGetBlackWhitePtBgr48(blackPt, whitePt), CGetTintedBlackWhitePtBase(tintingColor)
		{}

		bgr8 operator()(const uint8_t* p) const
		{
			const uint16_t* pus = (const uint16_t*)p;
			uint16_t pv = (uint16_t)((((int)(*pus)) + *(pus + 1) + *(pus + 2) + 1) / 3);
			float f = this->getBlkWhtGray16.GetPixelValueFloat(pv);
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	template <typename tStore>
	static void DoTintingBlackWhitePt(tStore store, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst,
		libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		ScopedBitmapLockerP lckSrc{ src };
		if (chInfo->enableTinting)
		{
			switch (src->GetPixelType())
			{
			case PixelType::Gray8:
			{
				CGetBlackWhitePtTintingGray8 t{ chInfo->tinting.color, chInfo->blackPoint, chInfo->whitePoint };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Gray16:
			{
				CGetBlackWhitePtTintingGray16 t{ chInfo->tinting.color, chInfo->blackPoint, chInfo->whitePoint };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr24:
			{
				CGetBlackWhitePtTintingBgr24 t{ chInfo->tinting.color, chInfo->blackPoint,chInfo->whitePoint };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr48:
			{
				CGetBlackWhitePtTintingBgr48 t{ chInfo->tinting.color, chInfo->blackPoint,chInfo->whitePoint };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			default:
				throw std::runtime_error("Not implemented for this pixeltype.");
			}
		}
		else
		{
			switch (src->GetPixelType())
			{
			case PixelType::Gray8:
			{
				CGetBlackWhitePtGray8 t{ chInfo->blackPoint,chInfo->whitePoint };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Gray16:
			{
				CGetBlackWhitePtGray16 t{ chInfo->blackPoint,chInfo->whitePoint };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr24:
			{
				CGetBlackWhitePtBgr24 t{ chInfo->blackPoint,chInfo->whitePoint };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr48:
			{
				CGetBlackWhitePtBgr48 t{ chInfo->blackPoint,chInfo->whitePoint };
				CopyTinting<tStore>(store, t, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			default:
				throw std::runtime_error("Not implemented for this pixeltype.");
			}
		}
	}

	static void DoTintingBlackWhitePtCopy(libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CStore store;
		DoTintingBlackWhitePt<CStore>(store, dest, lckDst, src, chInfo);
	}

	static void DoTintingBlackWhitePtAdd(libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CAdd store;
		DoTintingBlackWhitePt<CAdd>(store, dest, lckDst, src, chInfo);
	}

	static void DoTintingBlackWhitePtCopy(float weight, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CStoreWithWeight store(weight);
		DoTintingBlackWhitePt<CStoreWithWeight >(store, dest, lckDst, src, chInfo);
	}

	static void DoTintingBlackWhitePtAdd(float weight, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CAddWithWeight store(weight);
		DoTintingBlackWhitePt<CAddWithWeight>(store, dest, lckDst, src, chInfo);
	}
private:
	static int GetLutSize(PixelType pt)
	{
		switch (pt)
		{
		case PixelType::Gray8:
		case PixelType::Bgr24:
			return 256;
		case PixelType::Gray16:
		case PixelType::Bgr48:
			return 256 * 256;
		default:
			throw std::runtime_error("Pixeltype not supported");
		}
	}

	struct CGetGray8Lut : CGray8BytesPerPel
	{
		const uint8_t* pLut;
		explicit CGetGray8Lut(const uint8_t* pLut) : pLut(pLut) {}
		bgr8 operator()(const uint8_t* p) const
		{
			uint8_t l = *(this->pLut + *p);
			return bgr8{ l,l,l };
		}
	};

	struct CGetBgr24Lut : CBgr24BytesPerPel
	{
		const uint8_t* pLut;
		explicit CGetBgr24Lut(const uint8_t* pLut) : pLut(pLut) {}
		bgr8 operator()(const uint8_t* p) const
		{
			return bgr8{ *(this->pLut + *p),*(this->pLut + *(p + 1)),*(this->pLut + *(p + 2)) };
		}
	};

	struct CGetGray16Lut : CGray16BytesPerPel
	{
		const uint8_t* pLut;
		explicit CGetGray16Lut(const uint8_t* pLut) : pLut(pLut) {}
		bgr8 operator()(const uint8_t* p) const
		{
			uint16_t us = *((const uint16_t*)p);
			uint8_t l = *(this->pLut + us);
			return bgr8{ l,l,l };
		}
	};

	struct CGetBgr48Lut : CBgr48BytesPerPel
	{
		const uint8_t* pLut;
		explicit CGetBgr48Lut(const uint8_t* pLut) : pLut(pLut) {}
		bgr8 operator()(const uint8_t* p) const
		{
			const uint16_t* puv = (const uint16_t*)p;
			return bgr8{ *(this->pLut + *puv),*(this->pLut + *(puv + 1)),*(this->pLut + *(puv + 2)) };
		}
	};

	struct CGetGray8LutTinted : CGetTintedBase, CGray8BytesPerPel
	{
		const uint8_t* pLut;
		CGetGray8LutTinted(const uint8_t* pLut, Rgb8Color tintingColor) :CGetTintedBase(tintingColor), pLut(pLut) {}
		bgr8 operator()(const uint8_t* p) const
		{
			float f = *(this->pLut + *p);
			f /= 255;
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetGray16LutTinted : CGetTintedBase, CGray16BytesPerPel
	{
		const uint8_t* pLut;
		CGetGray16LutTinted(const uint8_t* pLut, Rgb8Color tintingColor) :CGetTintedBase(tintingColor), pLut(pLut) {}
		bgr8 operator()(const uint8_t* p) const
		{
			float f = *(this->pLut + *((const uint16_t*)p));
			f /= (256 - 1);
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetBgr24LutTinted : CGetTintedBase, CBgr24BytesPerPel
	{
		const uint8_t* pLut;
		CGetBgr24LutTinted(const uint8_t* pLut, Rgb8Color tintingColor) :CGetTintedBase(tintingColor), pLut(pLut) {}
		bgr8 operator()(const uint8_t* p) const
		{
			float f = (float)(((int)*(this->pLut + p[0])) + *(this->pLut + p[1])) + *(this->pLut + p[2]);
			f /= (3 * 255);
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	struct CGetBgr48LutTinted : CGetTintedBase, CBgr48BytesPerPel
	{
		const uint8_t* pLut;
		CGetBgr48LutTinted(const uint8_t* pLut, Rgb8Color tintingColor) :CGetTintedBase(tintingColor), pLut(pLut) {}
		bgr8 operator()(const uint8_t* p) const
		{
			const uint16_t* puv = (const uint16_t*)p;
			float f = (float)(((int)(this->pLut[puv[0]])) + this->pLut[puv[1]] + this->pLut[puv[2]]);
			f /= (3 * (256 - 1));
			return bgr8{ toByte(f*this->tintingColor.b),toByte(f*this->tintingColor.g),toByte(f*this->tintingColor.r) };
		}
	};

	template <typename tStore>
	static void DoLut(tStore store, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		auto pxTypeSrc = src->GetPixelType();
		ScopedBitmapLockerP lckSrc{ src };
		if (chInfo->enableTinting == false)
		{
			switch (pxTypeSrc)
			{
			case PixelType::Gray8:
			{
				CGetGray8Lut l(chInfo->ptrLookUpTable);
				CopyTinting<tStore>(store, l, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Gray16:
			{
				CGetGray16Lut l(chInfo->ptrLookUpTable);
				CopyTinting<tStore>(store, l, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr24:
			{
				CGetBgr24Lut l(chInfo->ptrLookUpTable);
				CopyTinting<tStore>(store, l, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr48:
			{
				CGetBgr48Lut l(chInfo->ptrLookUpTable);
				CopyTinting<tStore>(store, l, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			default:
				throw std::runtime_error("Pixeltype not supported");
			}
		}
		else
		{
			switch (pxTypeSrc)
			{
			case PixelType::Gray8:
			{
				CGetGray8LutTinted l(chInfo->ptrLookUpTable, chInfo->tinting.color);
				CopyTinting<tStore>(store, l, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Gray16:
			{
				CGetGray16LutTinted l(chInfo->ptrLookUpTable, chInfo->tinting.color);
				CopyTinting<tStore>(store, l, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr24:
			{
				CGetBgr24LutTinted l(chInfo->ptrLookUpTable, chInfo->tinting.color);
				CopyTinting<tStore>(store, l, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			case PixelType::Bgr48:
			{
				CGetBgr48LutTinted l(chInfo->ptrLookUpTable, chInfo->tinting.color);
				CopyTinting<tStore>(store, l, src->GetWidth(), src->GetHeight(), (uint8_t*)lckDst.ptrDataRoi, lckDst.stride, (const uint8_t*)lckSrc.ptrDataRoi, lckSrc.stride);
			}
			break;
			default:
				throw std::runtime_error("Pixeltype not supported");
			}
		}
	}

	static void DoLutCopy(libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CStore store;
		DoLut<CStore>(store, dest, lckDst, src, chInfo);
	}

	static void DoLutAdd(libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CAdd store;
		DoLut<CAdd>(store, dest, lckDst, src, chInfo);
	}

	static void DoLutCopy(float weight, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CStoreWithWeight store(weight);
		DoLut<CStoreWithWeight>(store, dest, lckDst, src, chInfo);
	}

	static void DoLutAdd(float weight, libCZI::IBitmapData* dest, const BitmapLockInfo& lckDst, libCZI::IBitmapData* src, const Compositors::ChannelInfo* chInfo)
	{
		CAddWithWeight store(weight);
		DoLut<CAddWithWeight>(store, dest, lckDst, src, chInfo);
	}

	static void CheckArguments(libCZI::IBitmapData* dest,
		int channelCount,
		libCZI::IBitmapData*const* srcBitmaps,
		const Compositors::ChannelInfo* channelInfos)
	{
		if (channelCount <= 0)
		{
			throw std::invalid_argument("channelCount must be >0");
		}

		if (dest->GetPixelType() != PixelType::Bgr24)
		{
			throw std::invalid_argument("Pixeltype of destination must be Bgr24");
		}

		if (srcBitmaps == nullptr)
		{
			throw std::invalid_argument("srcBitmaps==nullptr");
		}

		if (channelInfos == nullptr)
		{
			throw std::invalid_argument("channelInfos==nullptr");
		}

		for (int i = 0; i < channelCount; ++i)
		{
			if (srcBitmaps[i]->GetWidth() != dest->GetWidth() || srcBitmaps[i]->GetHeight() != dest->GetHeight())
			{
				throw std::invalid_argument("All the source bitmaps must have same width/height as destination.");
			}
		}

		for (int i = 0; i < channelCount; ++i)
		{
			if (srcBitmaps[i] == nullptr)
			{
				stringstream ss;
				ss << "index #" << i << " -> srcBitmaps[i]==nullptr";
				throw std::invalid_argument(ss.str());
			}

			if (channelInfos[i].lookUpTableElementCount > 0)
			{
				if (channelInfos[i].ptrLookUpTable == nullptr)
				{
					stringstream ss;
					ss << "index #" << i << " -> if lookUpTableElementCount>0, then ptrLookUpTable must be valid";
					throw std::invalid_argument(ss.str());
				}

				// GetLutSize will throw if pixeltype is not supported
				auto pxlType = srcBitmaps[i]->GetPixelType();
				int lutSize = GetLutSize(pxlType);

				if (channelInfos[i].lookUpTableElementCount != lutSize)
				{
					stringstream ss;
					ss << "index #" << i << " -> for pixeltype '" << libCZI::Utils::PixelTypeToInformalString(pxlType) << "' the size of look-up table must be " << lutSize << ".";
					throw std::invalid_argument(ss.str());
				}
			}
		}
	}

	/// Determine whether the weight must be handled. This is not the case if all weight are equal.
	/// If it is determined to be necessary, then we return the mean weight per channel (which is all weights
	/// summed up and divided by the number of channels).
	///
	/// \param channelCount					 Number of channels.
	/// \param channelInfos					 The channel infos.
	/// \param [out] meanWeightPerChannel	 If the return value is true, then this is set to the mean weight per channel.
	///
	/// \return True if the weight parameter needs to be handled, false otherwise. Only if true, the meanWeightPerChannel argument is valid.
	static bool CalcWeightSum(int channelCount, const Compositors::ChannelInfo* channelInfos, float& meanWeightPerChannel)
	{
		float sum = 0;
		for (int i = 0; i < channelCount; ++i)
		{
			float w = (channelInfos + i)->weight;
			if (std::isnan(w) || std::isinf(w))
			{
				if (GetSite()->IsEnabled(LOGLEVEL_WARNING))
				{
					stringstream str;
					str << "Channel#" << i << ": weight is not a valid number (is '" << w << "') -> using no weight at all";
					GetSite()->Log(LOGLEVEL_WARNING, str);
				}

				sum = std::numeric_limits<float>::quiet_NaN();
				break;
			}

			if (w < 0)
			{
				if (GetSite()->IsEnabled(LOGLEVEL_WARNING))
				{
					stringstream str;
					str << "Channel#" << i << ": weight is negative (is '" << w << "') -> using no weight at all";
					GetSite()->Log(LOGLEVEL_WARNING, str);
				}

				sum = std::numeric_limits<float>::quiet_NaN();
				break;
			}

			sum += w;
		}

		if (std::isnan(sum) || std::abs(sum) <= std::numeric_limits<float>::epsilon())
		{
			return false;
		}

		meanWeightPerChannel = sum / channelCount;

		// if the mean is different to at least one weight (i. e. if they are not all the same), than we have to 
		// honor the weights
		for (int i = 0; i < channelCount; ++i)
		{
			float diff = meanWeightPerChannel - (channelInfos + i)->weight;
			if (std::abs(diff) > std::numeric_limits<float>::epsilon())
			{
				return true;
			}
		}

		return false;
	}

public:
	static void ComposeMultiChannel_Bgr24(
		libCZI::IBitmapData* dest,
		int channelCount,
		libCZI::IBitmapData*const* srcBitmaps,
		const Compositors::ChannelInfo* channelInfos)
	{
		// check arguments
		CMultiChannelCompositor2::CheckArguments(dest, channelCount, srcBitmaps, channelInfos);

		ScopedBitmapLockerP lckDst{ dest };

		float meanWeightPerChannel;
		bool needToUseWeights = CalcWeightSum(channelCount, channelInfos, meanWeightPerChannel);

		if (!needToUseWeights)
		{
			for (int c = 0; c < channelCount; ++c)
			{
				if (IsUsingLut(channelInfos + c))
				{
					if (c == 0)
						DoLutCopy(dest, lckDst, srcBitmaps[c], channelInfos + c);
					else
						DoLutAdd(dest, lckDst, srcBitmaps[c], channelInfos + c);
				}
				else
				{
					if (IsBlackWhitePointUsed(channelInfos + c))
					{
						if (c == 0)
							DoTintingBlackWhitePtCopy(dest, lckDst, srcBitmaps[c], channelInfos + c);
						else
							DoTintingBlackWhitePtAdd(dest, lckDst, srcBitmaps[c], channelInfos + c);
					}
					else
					{
						if (c == 0)
							DoTintingCopy(dest, lckDst, srcBitmaps[c], channelInfos + c);
						else
							DoTintingAdd(dest, lckDst, srcBitmaps[c], channelInfos + c);
					}
				}
			}
		}
		else
		{
			for (int c = 0; c < channelCount; ++c)
			{
				float weightForChannel = (channelInfos + c)->weight / meanWeightPerChannel;
				if (IsUsingLut(channelInfos + c))
				{
					if (c == 0)
						DoLutCopy(weightForChannel, dest, lckDst, srcBitmaps[c], channelInfos + c);
					else
						DoLutAdd(weightForChannel, dest, lckDst, srcBitmaps[c], channelInfos + c);
				}
				else
				{
					if (IsBlackWhitePointUsed(channelInfos + c))
					{
						if (c == 0)
							DoTintingBlackWhitePtCopy(weightForChannel, dest, lckDst, srcBitmaps[c], channelInfos + c);
						else
							DoTintingBlackWhitePtAdd(weightForChannel, dest, lckDst, srcBitmaps[c], channelInfos + c);
					}
					else
					{
						if (c == 0)
							DoTintingCopy(weightForChannel, dest, lckDst, srcBitmaps[c], channelInfos + c);
						else
							DoTintingAdd(weightForChannel, dest, lckDst, srcBitmaps[c], channelInfos + c);
					}
				}
			}
		}
	}
};

/*static*/void Compositors::ComposeMultiChannel_Bgr24(
	libCZI::IBitmapData* dest,
	int channelCount,
	libCZI::IBitmapData*const* srcBitmaps,
	const ChannelInfo* channelInfos)
{
	CMultiChannelCompositor2::ComposeMultiChannel_Bgr24(dest, channelCount, srcBitmaps, channelInfos);
}

/*static*/std::shared_ptr<IBitmapData> Compositors::ComposeMultiChannel_Bgr24(
	int channelCount,
	libCZI::IBitmapData*const* srcBitmaps,
	const ChannelInfo* channelInfos)
{
	auto bmDest = GetSite()->CreateBitmap(PixelType::Bgr24, (*srcBitmaps)->GetWidth(), (*srcBitmaps)->GetHeight());
	Compositors::ComposeMultiChannel_Bgr24(bmDest.get(), channelCount, srcBitmaps, channelInfos);
	return bmDest;
}