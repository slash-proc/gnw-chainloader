import sys

def main():
    if len(sys.argv) < 2:
        print("Usage: python extract_unscii.py <unscii-8.hex>")
        sys.exit(1)
        
    with open(sys.argv[1], 'r') as f:
        lines = f.readlines()
        
    # Standard ASCII (Space to Z: 0x20 to 0x5A) -> 59 chars
    ascii_data = [[0]*8 for _ in range(59)]
    
    # Selected diacritics:
    # 0x0300 Grave, 0x0301 Acute, 0x0302 Circumflex, 0x0303 Tilde, 
    # 0x0304 Macron, 0x0308 Diaeresis, 0x030A Ring, 0x0327 Cedilla
    target_diacritics = [0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0308, 0x030A, 0x0327]
    diac_data = [[0]*8 for _ in range(8)]
    
    for line in lines:
        line = line.strip()
        if not line or ':' not in line: continue
        cp_hex, data_hex = line.split(':')
        cp = int(cp_hex, 16)
        data = [int(data_hex[i:i+2], 16) for i in range(0, 16, 2)]
        
        if 0x0020 <= cp <= 0x005A:
            ascii_data[cp - 0x0020] = data
        elif cp in target_diacritics:
            diac_data[target_diacritics.index(cp)] = data
            
    with open('src/chainloader/ui/gui_font.h', 'w') as f:
        f.write('#ifndef GUI_FONT_H\n#define GUI_FONT_H\n\n#include <stdint.h>\n\n')
        f.write('/* Standard ASCII Space (0x20) through Z (0x5A) */\n')
        f.write('extern const uint8_t gui_font_ascii[59][8];\n\n')
        f.write('/* Common combining diacritics */\n')
        f.write('extern const uint8_t gui_font_diacritics[8][8];\n')
        f.write('extern const uint16_t gui_font_diacritic_cps[8];\n\n')
        f.write('#endif // GUI_FONT_H\n')

    with open('src/chainloader/ui/gui_font.c', 'w') as f:
        f.write('#include "gui_font.h"\n\n')
        f.write('const uint8_t gui_font_ascii[59][8] = {\n')
        for i, row in enumerate(ascii_data):
            f.write('    { ' + ', '.join(f'0x{b:02X}' for b in row) + ' }, // ' + chr(0x20 + i) + '\n')
        f.write('};\n\n')
        
        f.write('const uint16_t gui_font_diacritic_cps[8] = {\n')
        f.write('    ' + ', '.join(f'0x{cp:04X}' for cp in target_diacritics) + '\n')
        f.write('};\n\n')
        
        f.write('const uint8_t gui_font_diacritics[8][8] = {\n')
        for i, row in enumerate(diac_data):
            f.write('    { ' + ', '.join(f'0x{b:02X}' for b in row) + ' }, // U+' + f'{target_diacritics[i]:04X}' + '\n')
        f.write('};\n')

if __name__ == '__main__':
    main()
