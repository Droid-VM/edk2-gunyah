
#include <Base.h>
#include <Library/BaseLib.h>

VOID SerialConvertWriteChar(IN UINT8 *Ch) {

}

VOID SerialConvertReadChar(IN UINT8 *Ch) {
  switch (*Ch) {
    case 0xa: //
      *Ch = 0xd;
      break;
    default:;
  }
}
