#define OEMRESOURCE

#include "CursorTypeConverter.hpp"
#include "CursorType.hpp"

DWORD CursorTypeConverter::toOCR(CursorType type)
{
    switch (type)
    {
        case CursorType::Normal:
            return OCR_NORMAL;

        #ifdef OCR_HELP
        case CursorType::Help:
            return OCR_HELP;
        #endif

        case CursorType::AppStarting:
            return OCR_APPSTARTING;

        case CursorType::Wait:
            return OCR_WAIT;

        case CursorType::Cross:
            return OCR_CROSS;

        case CursorType::IBeam:
            return OCR_IBEAM;

        case CursorType::NWPen:
            return OCR_ICON;

        case CursorType::No:
            return OCR_NO;

        case CursorType::SizeAll:
            return OCR_SIZEALL;

        case CursorType::SizeNESW:
            return OCR_SIZENESW;

        case CursorType::SizeNS:
            return OCR_SIZENS;

        case CursorType::SizeNWSE:
            return OCR_SIZENWSE;

        case CursorType::SizeWE:
            return OCR_SIZEWE;

        case CursorType::UpArrow:
            return OCR_UP;

        case CursorType::Hand:
            return OCR_HAND;

        #ifdef OCR_PIN
        case CursorType::Pin:
            return OCR_PIN;
        #endif

        #ifdef OCR_PERSON
        case CursorType::Person:
            return OCR_PERSON;
        #endif
    }

    return OCR_NORMAL;
}




