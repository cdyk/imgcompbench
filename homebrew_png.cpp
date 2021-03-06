#include <zlib.h>
#include "timer.hpp"
#include <xmmintrin.h>
#include <emmintrin.h>
#include <smmintrin.h>
#include <tmmintrin.h>
#include <vector>
#include <fstream>
#include <iostream>
#include "ThreadPool.hpp"
#include "BitPusher.hpp"
#include "Adler32.hpp"
#include "LZEncoder.hpp"
#include "HuffEncode.hpp"
#include "ScanlineFilter.hpp"

//#define PARALLEL

static std::vector<unsigned long> crc_table;

void
createCRCTable( )
{
    crc_table.resize( 256 );
    for( int j=0; j<256; j++) {
        unsigned long int c = j;
        for( int i=0; i<8; i++) {
            if( c & 0x1 ) {
                c = 0xedb88320ul ^ (c>>1);
            }
            else {
                c = c>>1;
            }
        }
        crc_table[j] = c;
    }    
}

void
writeSignature( std::ofstream& file )
{
    unsigned char signature[8] = 
    {
        137, 80, 78, 71, 13, 10, 26, 10
    };
    file.write( reinterpret_cast<char*>( signature ), sizeof(signature ) );    
}


void
encodeCount( unsigned int& bits, unsigned int& bits_n, unsigned int count )
{
    //std::cerr << "count=" << count << "  ";

    
    uint q = 0;
    
    if( count < 3 ) {
        abort();
        // no copies, 1 or 2 never occurs.
    }
    else if( count < 11 ) {
        unsigned int t = count-3;
        t = (t&0x07u)+((257-256)<<0);
        bits = (bits<<7) | t;           // 7-bit count code
        bits_n += 7;
    }
    else if( count < 19 ) {     // count=11..18, code=265..268, 1 extra bit
        int t = count - 11;
        q = (t&0x01u)<<(8-1);
        t = (t&0x06u) + ((265-256)<<1);
        bits = (bits<<8) | t;
        bits_n += 8;
    }
    else if( count < 35 ) {     // count=19..34, code=269..272, 2 extra bits
        int t = count - 19;
        q = (t&0x03u)<<(8-2);
        t = (t&0x0Cu) + ((269-256)<<2);
        bits = (bits<<9) | t;
        bits_n += 9;
    }
    else if( count < 67 ) {     // c=35..66, code=273..276, 3 extra bits
        int t = count - 35;
        q = (t&0x07u)<<(8-3);
        t = (t&0x18u) + ((273-256)<<3);
        bits = (bits<<10) | t;
        bits_n += 10;
    }    
    else if( count < 115 ) {    // c=67..114, 7-bit code=277..279, 4 extra bits
        int t = count - 67;
        q = (t&0x0Fu)<<(8-4);
        t = (t&0x30u) + ((277-256)<<4);
        bits = (bits<<11) | t;
        bits_n += 11;
    }
    else if( count < 131 ) {    // c=115..130, 8-bit code=280, 4 extra bits
        int t = count - 115;
        q = (t&0x0Fu)<<(8-4);
        t = (t&0x30u) + ((280-280+0xc0)<<4);
        bits = (bits<<12) | t;
        bits_n += 12;
    }
    else if( count < 258 ) {    // c=131..257, code=281..284, 5 extra bits
        int t = count - 131;
        bits = (bits<<12) | ((t&(0xff<<5))+((281-280+0xc0)<<5));
        q = (t&0x1Fu)<<(8-5);
        bits_n += 13;
    }
    else if( count < 259 ) {
        bits = (bits<<8) | (285-280+0xc0);
        bits_n += 8;
    }
    else {
        std::cerr << "unsupported count " << count << "\n";
        abort();
    }
    
    q = (( q & 0x55u)<<1) | ((q>>1)&0x55u);
    q = (( q & 0x33u)<<2) | ((q>>2)&0x33u);
    q = (( q & 0x0Fu)<<4) | ((q>>4)&0x0Fu);
    bits = bits | q;
}


void
encodeDistance( unsigned int& bits, unsigned int& bits_n, unsigned int distance )
{
    //std::cerr << "distance=" << distance << "  ";
    
    uint q = 0;
    
    if ( distance < 1 ) {
        std::cerr << "unsupported distance " << distance << "\n";
        abort();
    }
    else if( distance < 5 ) {   // d=1..4, code=0..3, 0 extra bits
        bits = (bits<<5) | (distance-1);
        bits_n += 5;
    }
    else if( distance < 9 ) {   // d=5..8, code=4..5, 1 extra bit
        unsigned int t = distance - 5;
        bits = (bits<<6) | ((t&(0x1f<<1))+(4<<1));
        q = (t&1u)<<(32-1);
        bits_n += 6;
    }
    else if( distance < 17 ) {      // d=9..16, code=6..7, 2 extra bits
        unsigned int t = distance - 9;
        bits = (bits<<7) | ((t&(0x1<<2))+(6<<2));
        q = (t&3u)<<(32-2);
        bits_n += 7;
    }
    else if( distance < 33 ) {      // d=17..32, code=8..9, 3 extra bits
        unsigned int t = distance - 17;
        bits = (bits<<8) | ((t&(0x1<<3))+(8<<3));
        q = (t&7u)<<(32-3);
        bits_n += 8;
    }
    else if( distance < 65 ) {      // d=33..64, code=10..11, 4 extra bits
        unsigned int t = distance - 33;
        bits = (bits<<9) | ((t&(0x1<<4))+(10<<4));
        q = (t&0xFu)<<(32-4);
        bits_n += 9;
    }
    else if( distance < 129 ) {     // d=65..128, code=12..13, 5 extra bits
        unsigned int t = distance - 65;
        bits = (bits<<10) | ((t&(0x1<<5))+(12<<5));
        q = (t&0x1Fu)<<(32-5);
        bits_n += 10;
    }
    else if( distance < 257 ) {     // d=129..256, code=14,15, 6 extra bits
        unsigned int t = distance - 129;
        q = (t&0x3Fu)<<(32-6);
        t = (t&0x40u)+(14<<6);
        bits = (bits<<11) | t;
        bits_n += 11;
    }
    else if( distance < 513 ) {     // d=257..512, code 16..17, 7 extra bits
        unsigned int t = distance - 257;
        q = (t&0x7Fu)<<(32-7);
        t = (t&0x80u)+(16<<7);
        bits = (bits<<12) | t;
        bits_n += 12;
    }
    else if( distance < 1025 ) {     // d=257..512, code 18..19, 8 extra bits
        unsigned int t = distance - 513;
        q = (t&0x0FFu)<<(32-8);
        t = (t&0x100u)+(18<<8);
        bits = (bits<<13) | t;
        bits_n += 13;
    }
    else if( distance < 2049 ) {    // d=1025..2048, code 20..21, 9 extra bits
        unsigned int t = distance - 1025;
        q = (t&0x1FFu)<<(32-9);
        t = (t&0x200u)+(20<<9);
        bits = (bits<<14) | t;
        bits_n += 14;
    }
    else if( distance < 4097 ) {    // d=2049..4096, code 22..23, 10 extra bits
        unsigned int t = distance - 2049;
        q = (t&0x3FFu)<<(32-10);
        t = (t&0x400u)+(22<<10);
        bits = (bits<<15) | t;
        bits_n += 15;
    }
    else if( distance < 8193 ) {    // d=4097..8192, code 24..25, 11 extra bits
        unsigned int t = distance - 4097;
        q = (t&0x7FFu)<<(32-11);
        t = (t&0x800u)+(24<<11);
        bits = (bits<<16) | t;
        bits_n += 16;
    }
    else if( distance < 16385 ) {   // d=8193..16384, code 26..27, 12 extra bits
        unsigned int t = distance - 8193;
        q = (t&0x0FFFu)<<(32-12);
        t = (t&0x1000u)+(26<<12);
        bits = (bits<<17) | t;
        bits_n += 17;
    }
    else if( distance < 32769 ) {   // d=16385..32768, code 28..29, 13 extra bits
        unsigned int t = distance - 16385;
        q = (t&0x1FFFu)<<(32-13);
        t = (t&0x2000u)+(28<<13);
        bits = (bits<<18) | t;
        bits_n += 18;
    }
    else {
        std::cerr << "Illegal " << distance << "\n";
        abort();
    }

    q = (( q & 0x55555555u)<< 1) | ((q>> 1)&0x55555555u);
    q = (( q & 0x33333333u)<< 2) | ((q>> 2)&0x33333333u);
    q = (( q & 0x0F0F0F0Fu)<< 4) | ((q>> 4)&0x0F0F0F0Fu);
    q = (( q & 0x00FF00FFu)<< 8) | ((q>> 8)&0x00FF00FFu);
    q = (( q & 0x0000FFFFu)<<16) | ((q>>16)&0x0000FFFFu);
    bits = bits | q;
}




void
encodeLiteralTriplet( unsigned int& bits, unsigned int& bits_n, unsigned int rgb )
{
    //std::cerr << "literal=" << rgb << "  ";
    
    unsigned int t = (rgb >> 16) & 0xffu;
    if( t < 144 ) {
        bits = (t + 48);
        bits_n = 8;
    }
    else {
        bits = (t + 256);
        bits_n = 9;
    }

    t = (rgb>>8)& 0xffu;
    if( t < 144 ) {
        bits = (bits<<8) | (t + 48);
        bits_n += 8;
    }
    else {
        bits = (bits<<9) | (t + 256);
        bits_n += 9;
    }

    t = (rgb)& 0xffu;
    if( t < 144 ) {
        bits = (bits<<8) | (t + 48);
        bits_n += 8;
    }
    else {
        bits = (bits<<9) | (t + 256);
        bits_n += 9;
    }
}


unsigned long int
CRC( const std::vector<unsigned long>& crc_table, const unsigned char* p, size_t length )
{
    size_t i;
    unsigned long int crc = 0xffffffffl;
    for(i=0; i<length; i++) {
        unsigned int ix = ( p[i]^crc ) & 0xff;
        crc = crc_table[ix]^((crc>>8));
    }
    return ~crc;
}




void
writeIDAT3( std::ofstream& file, const std::vector<char>& img, const std::vector<unsigned long>& crc_table, int WIDTH, int HEIGHT  )
{
    
    
    std::vector<unsigned char> IDAT(8);
    // IDAT chunk header
    IDAT[4] = 'I';
    IDAT[5] = 'D';
    IDAT[6] = 'A';
    IDAT[7] = 'T';

    // --- create deflate chunk ------------------------------------------------
    IDAT.push_back(  8 + (7<<4) );  // CM=8=deflate, CINFO=7=32K window size = 112
    IDAT.push_back( 94 /* 28*/ );           // FLG
    
    unsigned int dat_size;

    
    unsigned int s1 = 1;
    unsigned int s2 = 0;
    {
        BitPusher pusher( IDAT );
        pusher.pushBitsReverse( 6, 3 );    // 5 = 101

        std::vector<unsigned int> buffer( WIDTH*5, ~0u );
        
        unsigned int* zrows[4] = {
            buffer.data(),
            buffer.data()+WIDTH,
            buffer.data()+2*WIDTH,
            buffer.data()+3*WIDTH,
        };
        
        unsigned int o = 0;

        
        for( int j=0; j<HEIGHT; j++ ) {
            unsigned int* rows[4] = {
                zrows[ (j+3)&1 ],
                zrows[ (j+2)&1 ],
                zrows[ (j+1)&1 ],
                zrows[ j&1 ]
            };
            
            
            //            unsigned int* p_row = rows[ j&1 ];
            //unsigned int* c_row = rows[ (j+1)&1 ];

            pusher.pushBitsReverse( 0 + 48, 8 );    // Push png scanline filter
            s1 += 0;                                // update adler 1 & 2
            s2 += s1;                          

            int match_src_i = 0;
            int match_src_j = 0;
            int match_dst_i = 0;
            int match_dst_o = 0; // used for debugging
            int match_length = 0;
            //std::cerr << o << "---\n";
            o++;

            for(int i=0; i<WIDTH; i++ ) {
                unsigned int R = img[ 3*(WIDTH*j + i ) + 0 ];
                unsigned int G = img[ 3*(WIDTH*j + i ) + 1 ];
                unsigned int B = img[ 3*(WIDTH*j + i ) + 2 ];
                unsigned int RGB = (R<<16) | (G<<8) | B;

                s1 = (s1 + R);
                s2 = (s2 + s1);
                s1 = (s1 + G);
                s2 = (s2 + s1);
                s1 = (s1 + B);
                s2 = (s2 + s1);
                
                //std::cerr << o << ":\t" << RGB << "\t";
                rows[0][i] = RGB;

                bool redo;
                
                do {
                    redo = false;
                    bool emit_match = false;
                    bool emit_verbatim = false;
                    
                    if( match_length == 0 ) {
                        // We have no running matching, try to find candidate
                        int k;
                        

                        match_src_j = 0;
                        for( k=i-1; (k>=0)&&(rows[0][k] != RGB); k--) {}

                        if( k < 0 ) {
                            match_src_j = 1;
                            for( k=WIDTH-1; (k>=0)&&(rows[1][k] != RGB); k--) {}
                        }
                        
                        if( k >= 0 ) {
                            // Found match,
                            match_src_i = k;
                            match_dst_i = i;
                            match_dst_o = o;
                            match_length = 1;
                            if( i == WIDTH -1 ) {
                                emit_match = true;
                            }
                        }
                        else {
                            emit_verbatim = true;
                        }
                    }
                    else {
                        // We are matching

                        if( match_length >= 86 ) {
                            // Max matchlength is 86*3=258, flush and continue
                            emit_match = true;
                            redo = true;
                        }
                        
                        else if( (match_src_i + match_length < WIDTH ) &&    // don't match outside scanline
                                (rows[match_src_j][match_src_i+match_length] == RGB) )  // check if matching
                        {
                            match_length = match_length + 1;
                            if( i == WIDTH -1 ) {
                                emit_match = true;
                            }
                        }
                        else {
                            emit_match = true;
                            redo = true;

                            // try to find new match source
                            int k = match_src_i-1;
                            for(int m=match_src_j; (emit_match) && (m<2); m++ ) {
                                for(; (emit_match) && (k>=0); k--) {
                                    bool fail = false;
                                    for(int l=0; l<=match_length; l++) {
                                        if( rows[0][ match_dst_i+l] != rows[m][ k+l] ) {
                                            fail = true;
                                            break;
                                        }
                                    }
                                    if( !fail ) {
                                        match_src_j = m;
                                        match_src_i = k;
                                        match_length = match_length + 1;
                                        emit_match = false;
                                        redo = false;
                                        break;
                                    }
                                }
                                k = WIDTH-1;
                            }

                        }
                    }
                    
                    if( ((i == WIDTH-1) && (match_length)) || emit_match ) {
                        unsigned int bits = 0;
                        unsigned int bits_n = 0;
                        
                        unsigned int count = 3*match_length;
                        encodeCount( bits, bits_n, count );
                        pusher.pushBitsReverse( bits, bits_n );
                        
                        bits = 0;
                        bits_n = 0;
                        unsigned int distance = 3*(match_dst_i-match_src_i);
                        
                        if( match_src_j > 0 ) {
                            distance += 3*WIDTH+1;
                        }
                        
                        encodeDistance( bits, bits_n, distance );
                        pusher.pushBitsReverse( bits, bits_n );
                        match_length = 0;
                    }
                    
                    if( emit_verbatim ) {
                        unsigned int bits = 0;
                        unsigned int bits_n = 0;
                        encodeLiteralTriplet( bits, bits_n, RGB );
                        pusher.pushBitsReverse( bits, bits_n );
                    }
                }
                while( redo );
                    
                o+=3;
            }
            s1 = s1 % 65521;
            s2 = s2 % 65521;
           
        }
        pusher.pushBits( 0, 7 );    // EOB 
    }
    unsigned int adler = (s2<<16) + s1;
    
    IDAT.push_back( ((adler)>>24)&0xffu ); // Adler32
    IDAT.push_back( ((adler)>>16)&0xffu );
    IDAT.push_back( ((adler)>> 8)&0xffu );
    IDAT.push_back( ((adler)>> 0)&0xffu );

    // --- end deflate chunk --------------------------------------------------
    
    
    // Update PNG chunk content size for IDAT
    dat_size = IDAT.size()-8u;
    IDAT[0] = ((dat_size)>>24)&0xffu;
    IDAT[1] = ((dat_size)>>16)&0xffu;
    IDAT[2] = ((dat_size)>>8)&0xffu;
    IDAT[3] = ((dat_size)>>0)&0xffu;

    unsigned long crc = CRC( crc_table, IDAT.data()+4, dat_size+4 );
    IDAT.resize( IDAT.size()+4u );  // make room for CRC
    IDAT[dat_size+8]  = ((crc)>>24)&0xffu;
    IDAT[dat_size+9]  = ((crc)>>16)&0xffu;
    IDAT[dat_size+10] = ((crc)>>8)&0xffu;
    IDAT[dat_size+11] = ((crc)>>0)&0xffu;

    
    

#if 0
    if( 1) {
        std::vector<unsigned char> quux(10*1024*1024);

        z_stream stream;
        int err;
    
        stream.next_in = (z_const Bytef *)IDAT.data()+8;
        stream.avail_in = (uInt)IDAT.size()-8;
        stream.next_out = quux.data();
        stream.avail_out = quux.size();
        stream.zalloc = (alloc_func)0;
        stream.zfree = (free_func)0;

        err = inflateInit(&stream);
        if (err != Z_OK) {
            std::cerr << "inflateInit failed: " << err << "\n";
            abort();
        }
       
        err = inflate(&stream, Z_FINISH);

        if( stream.msg != NULL ) {
            std::cerr << stream.msg << "\n";
        }
        

        uLongf quux_size = quux.size();
        err = uncompress( quux.data(), &quux_size, IDAT.data() + 8, IDAT.size() - 8 );
        if( err != Z_OK ) {
            std::cerr << "uncompress="
                      << err
                      << "\n";
        }
        if( quux_size != ((3*WIDTH+1)*HEIGHT) ) {
            std::cerr << "uncompress_size="  << quux_size
                      << ", should be=" << ((3*WIDTH+1)*HEIGHT) << "\n";
        }
    }
#endif
    
    file.write( reinterpret_cast<char*>( IDAT.data() ), dat_size+12 );
}
void
writeIHDR( std::ofstream& file, const std::vector<unsigned long>& crc_table, int WIDTH, int HEIGHT )
{
    // IHDR chunk, 13 + 12 (length, type, crc) = 25 bytes
    unsigned char IHDR[ 25 ] = 
    {
        // Chunk length (4 bytes)
        ((13)>>24)&0xffu,((13)>>16)&0xffu, ((13)>>8)&0xffu, ((13)>>0)&0xffu,
        // Chunk type (4 bytes)
        'I', 'H', 'D', 'R',
        // Image width (4 bytes)
        ((WIDTH)>>24)&0xffu,((WIDTH)>>16)&0xffu, ((WIDTH)>>8)&0xffu, ((WIDTH)>>0)&0xffu,
        // Image height
        ((HEIGHT)>>24)&0xffu,((HEIGHT)>>16)&0xffu, ((HEIGHT)>>8)&0xffu, ((HEIGHT)>>0)&0xffu,
        // bits per channel, RGB triple, ..., .., image not interlaced (5 bytes)
        8, 2, 0, 0, 0,
        // CRC of 13+4 bytes
        0, 0, 0, 0
    };
    unsigned long crc = CRC( crc_table, IHDR+4, 13+4 );
    IHDR[21] = ((crc)>>24)&0xffu;    // image width
    IHDR[22] = ((crc)>>16)&0xffu;
    IHDR[23] = ((crc)>>8)&0xffu;
    IHDR[24] = ((crc)>>0)&0xffu;
    
    file.write( reinterpret_cast<char*>( IHDR ), sizeof(IHDR) );
}



void
writeIDAT2( std::ofstream& file, const std::vector<char>& img, const std::vector<unsigned long>& crc_table, int WIDTH, int HEIGHT  )
{
    
    
    std::vector<unsigned char> IDAT(8);
    // IDAT chunk header
    IDAT[4] = 'I';
    IDAT[5] = 'D';
    IDAT[6] = 'A';
    IDAT[7] = 'T';

    // --- create deflate chunk ------------------------------------------------
    IDAT.push_back(  8 + (7<<4) );  // CM=8=deflate, CINFO=7=32K window size = 112
    IDAT.push_back( 94 /* 28*/ );           // FLG
    
    unsigned int dat_size;
    unsigned int s1 = 1;
    unsigned int s2 = 0;
    {
        BitPusher pusher( IDAT );
        pusher.pushBitsReverse( 6, 3 );    // 5 = 101

        for( int j=0; j<HEIGHT; j++) {
            
            // push scan-line filter type
            pusher.pushBitsReverse( 1 + 48, 8 );    // Push a 1 (diff with left)
            s1 += 1;                                // update adler 1 & 2
            s2 += s1;                          

#if 1
            unsigned int trgb_p = 0xffffffff;
            unsigned int c = 0;
            for(int i=0; i<WIDTH; i++ ) {
                
                unsigned int trgb_l = 0;
                for(int k=0; k<3; k++) {
                    unsigned int t = (img[ 3*WIDTH*j + 3*i + k ]
                                   - (i==0?0:img[3*WIDTH*j+3*(i-1)+k ] )) & 0xffu;

                    s1 = (s1 + t);                  // update adler 1 & 2
                    s2 = (s2 + s1);
                    trgb_l = (trgb_l<<8) | t;
                }
                if( (i==0) || (i==WIDTH-1) || (trgb_l != trgb_p) || ( c >= 66 ) ) {
                    // flush copies
                    if( c == 0 ) {
                        // no copies, 1 or 2 never occurs.
                    }
                    else if( c < 11 ) {
                        pusher.pushBitsReverse( c-2, 7 );
                        pusher.pushBitsReverse( 2, 5 );
                    }
                    else if( c < 19 ) {
                        int t = c - 11;
                        pusher.pushBitsReverse( (t>>1)+9, 7 );
                        pusher.pushBitsReverse( (t&1), 1 );
                        pusher.pushBitsReverse( 2, 5 );
                    }
                    else if( c < 35 ) {
                        int t = c - 19;
                        pusher.pushBitsReverse( (t>>2)+13, 7 );
                        pusher.pushBits( (t&3), 2 );
                        pusher.pushBitsReverse( 2, 5 );
                    }
                    else if( c < 67 ) {
                        int t = c - 35;
                        pusher.pushBitsReverse( (t>>3)+17, 7 );
                        pusher.pushBits( (t&7), 3 );
                        pusher.pushBitsReverse( 2, 5 );
                    }
                    c = 0;
                   
                    
                    // need to write literal
                    int r = (trgb_l >> 16)& 0xffu;
                    if( r < 144 ) {
                        pusher.pushBitsReverse( r + 48, 8 );
                    }
                    else {
                        pusher.pushBitsReverse( r + (400-144), 9 ); 
                    }
                    int g = (trgb_l >> 8)& 0xffu;
                    if( g < 144 ) {
                        pusher.pushBitsReverse( g + 48, 8 );
                    }
                    else {
                        pusher.pushBitsReverse( g + (400-144), 9 ); 
                    }
                    int b = (trgb_l >> 0)& 0xffu;
                    if( b < 144 ) {
                        pusher.pushBitsReverse( b + 48, 8 );
                    }
                    else {
                        pusher.pushBitsReverse( b + (400-144), 9 ); 
                    }
                    trgb_p = trgb_l;            
                }
                else {
                    c+=3;
                }
            }
#else            
            for(int i=0; i<WIDTH; i++) {
                



                
                for(int k=0; k<3; k++ ) {
                    int q = img[ 3*WIDTH*j + 3*i + k ];
                    s1 = (s1 + q);                  // update adler 1 & 2
                    s2 = (s2 + s1);

                    if( q < 144 ) {
                        pusher.pushBitsReverse( q + 48, 8 );            // 48 = 00110000
                    }
                    else {
                        pusher.pushBitsReverse( q + (400-144), 9 );     // 400 = 110010000
                    }
                }
            }
#endif
            
            // We can do up to 5552 iterations before we need to run the modulo,
            // see comment on NMAX adler32.c in zlib.
            s1 = s1 % 65521;
            s2 = s2 % 65521;
        }
        pusher.pushBits( 0, 7 );    // EOB 
    }
    unsigned int adler = (s2<<16) + s1;
    
    IDAT.push_back( ((adler)>>24)&0xffu ); // Adler32
    IDAT.push_back( ((adler)>>16)&0xffu );
    IDAT.push_back( ((adler)>> 8)&0xffu );
    IDAT.push_back( ((adler)>> 0)&0xffu );

    // --- end deflate chunk --------------------------------------------------


    
    
    // Update PNG chunk content size for IDAT
    dat_size = IDAT.size()-8u;
    IDAT[0] = ((dat_size)>>24)&0xffu;
    IDAT[1] = ((dat_size)>>16)&0xffu;
    IDAT[2] = ((dat_size)>>8)&0xffu;
    IDAT[3] = ((dat_size)>>0)&0xffu;

    unsigned long crc = CRC( crc_table, IDAT.data()+4, dat_size+4 );
    IDAT.resize( IDAT.size()+4u );  // make room for CRC
    IDAT[dat_size+8]  = ((crc)>>24)&0xffu;
    IDAT[dat_size+9]  = ((crc)>>16)&0xffu;
    IDAT[dat_size+10] = ((crc)>>8)&0xffu;
    IDAT[dat_size+11] = ((crc)>>0)&0xffu;

    
    
#if 0
    if( 1) {
        std::vector<unsigned char> quux(10*1024*1024);

        z_stream stream;
        int err;
    
        stream.next_in = (z_const Bytef *)IDAT.data()+8;
        stream.avail_in = (uInt)IDAT.size()-8;
        stream.next_out = quux.data();
        stream.avail_out = quux.size();
        stream.zalloc = (alloc_func)0;
        stream.zfree = (free_func)0;

        err = inflateInit(&stream);
        if (err != Z_OK) {
            std::cerr << "inflateInit failed: " << err << "\n";
            abort();
        }
       
        err = inflate(&stream, Z_FINISH);

        if( stream.msg != NULL ) {
            std::cerr << stream.msg << "\n";
        }
        

        uLongf quux_size = quux.size();
        err = uncompress( quux.data(), &quux_size, IDAT.data() + 8, IDAT.size() - 8 );
        if( err != Z_OK ) {
            std::cerr << "uncompress="
                      << err
                      << "\n";
        }
        if( quux_size != ((3*WIDTH+1)*HEIGHT) ) {
            std::cerr << "uncompress_size="  << quux_size
                      << ", should be=" << ((3*WIDTH+1)*HEIGHT) << "\n";
        }
        
    }
#endif
    
    file.write( reinterpret_cast<char*>( IDAT.data() ), dat_size+12 );
}


struct CacheItem
{
    int         m_literal;
    int         m_next;
};

static inline unsigned char
hash( unsigned char a, unsigned char b, unsigned char c )
{
    return a | ((b<<3) | (b>>5)) | ((c<<5) | (c>>3));
}


int
lengthOfMatch( const unsigned char* a,
               const unsigned char* b,
               const int N )
{

#if 1
    for(int i=0; i<N; i+=16 ) {
        __m128i _a = _mm_lddqu_si128( (__m128i const*)(a+i) );
        __m128i _b = _mm_lddqu_si128( (__m128i const*)(b+i) );
        unsigned int q = _mm_movemask_epi8( _mm_cmpeq_epi8( _a, _b ) );
        if( q != 0xffffu ) {
            int l = i + __builtin_ctz( ~q );
            return std::min( N, l );
        }
    }
#else
    for( int i=0; i<N; i++ ) {
        if( *a++ != *b++ ) {
            return i;
        }
    }
#endif
    return N;
}



class IDAT4Worker : public JobInterface
{
public:
    IDAT4Worker( unsigned int* code_stream_p,
                 unsigned int* code_stream_n,
                 unsigned char* filtered,
                 unsigned char* image,
                 unsigned int width,
                 unsigned int height )
        : m_code_stream_p( code_stream_p ),
          m_code_stream_n( code_stream_n ),
          m_filtered( filtered ),
          m_image( image ),
          m_width( width ),
          m_height( height )
    {}

    void
    run()
    {
        filterScanlines( m_filtered, m_image, m_width, m_height );
        *m_code_stream_n = encodeLZ( m_code_stream_p, m_filtered, (3*m_width+1)*m_height );
    }

protected:
    unsigned int*   m_code_stream_p;
    unsigned int*   m_code_stream_n;
    unsigned char*  m_filtered;
    unsigned char*  m_image;
    unsigned int    m_width;
    unsigned int    m_height;

};

class Adler32Job : public JobInterface
{
public:
    Adler32Job( unsigned int* adler32,
                unsigned char* data,
                unsigned int N )
        : m_adler32( adler32 ),
          m_data( data ),
          m_N( N )
    {}

    void
    run()
    {
        *m_adler32 = computeAdler32SSE( m_data, m_N );
    }

protected:
    unsigned int*   m_adler32;
    unsigned char*  m_data;
    unsigned int    m_N;
};

class HuffCodeJob : public JobInterface
{
public:
    HuffCodeJob( std::vector<unsigned char>& output,
                 unsigned int** code_stream_p,
                 unsigned int*  code_stream_N,
                 unsigned int   code_streams )
        : m_output( output ),
          m_code_stream_p( code_stream_p ),
          m_code_stream_N( code_stream_N ),
          m_code_streams( code_streams )
    {}

    void
    run()
    {
        encodeHuffman( m_output, m_code_stream_p, m_code_stream_N, m_code_streams );
    }

protected:
    std::vector<unsigned char>& m_output;
    unsigned int** m_code_stream_p;
    unsigned int*  m_code_stream_N;
    unsigned int   m_code_streams;
};



void
writeIDAT4MC( ThreadPool *thread_pool, std::ofstream& file, const std::vector<char>& img, const std::vector<unsigned long>& crc_table, int WIDTH, int HEIGHT  )
{
    int T = (thread_pool->workers()+1);


    TimeStamp T0;
    unsigned int adler;
    unsigned int filtered_size = (3*WIDTH+1)*HEIGHT;
    unsigned char* filtered = (unsigned char*)malloc( sizeof(unsigned char)*filtered_size );
    unsigned int* codestream = (unsigned int*)malloc(sizeof(unsigned int)*filtered_size );

    CompletionToken tokenA, tokenB;

    unsigned int* _codestream_p[ T ];
    unsigned int  _codestream_n[ T ];
    for( int t=0; t<T; t++ ) {
        int a = (t*HEIGHT)/T;
        int b = ((t+1)*HEIGHT)/T;


        _codestream_p[ t ] = codestream + (3*WIDTH+1)*a;
        _codestream_n[ t ] = 0;

        thread_pool->addJob( new IDAT4Worker( _codestream_p[ t ],
                                              _codestream_n + t,
                                              filtered + (3*WIDTH+1)*a,
                                              (unsigned char*)(img.data()) + 3*WIDTH*a,
                                              WIDTH, b-a ),
                             &tokenA );
    }
    thread_pool->wait( &tokenA );
    TimeStamp T1;
    thread_pool->addJob( new Adler32Job( &adler, filtered, filtered_size ),
                         &tokenB );

    std::vector<unsigned char> IDAT(8);
    IDAT[4] = 'I';
    IDAT[5] = 'D';
    IDAT[6] = 'A';
    IDAT[7] = 'T';
    thread_pool->addJob( new HuffCodeJob( IDAT, _codestream_p, _codestream_n, T ),
                         &tokenB );

    thread_pool->wait( &tokenB );
    TimeStamp T2;


    TimeStamp T4;


    IDAT.push_back( ((adler)>>24)&0xffu ); // Adler32
    IDAT.push_back( ((adler)>>16)&0xffu );
    IDAT.push_back( ((adler)>> 8)&0xffu );
    IDAT.push_back( ((adler)>> 0)&0xffu );

    // --- end deflate chunk --------------------------------------------------

    // Update PNG chunk content size for IDAT
    int dat_size = IDAT.size()-8u;
    IDAT[0] = ((dat_size)>>24)&0xffu;
    IDAT[1] = ((dat_size)>>16)&0xffu;
    IDAT[2] = ((dat_size)>>8)&0xffu;
    IDAT[3] = ((dat_size)>>0)&0xffu;

    unsigned long crc = CRC( crc_table, IDAT.data()+4, dat_size+4 );
    IDAT.resize( IDAT.size()+4u );  // make room for CRC
    IDAT[dat_size+8]  = ((crc)>>24)&0xffu;
    IDAT[dat_size+9]  = ((crc)>>16)&0xffu;
    IDAT[dat_size+10] = ((crc)>>8)&0xffu;
    IDAT[dat_size+11] = ((crc)>>0)&0xffu;

    TimeStamp T5;

    file.write( reinterpret_cast<char*>( IDAT.data() ), IDAT.size() );

    TimeStamp T6;

#if 1
    if( 1) {
        std::vector<unsigned char> quux(10*1024*1024);

        z_stream stream;
        int err;

        stream.next_in = (z_const Bytef *)IDAT.data()+8;
        stream.avail_in = (uInt)IDAT.size()-8;
        stream.next_out = quux.data();
        stream.avail_out = quux.size();
        stream.zalloc = (alloc_func)0;
        stream.zfree = (free_func)0;

        err = inflateInit(&stream);
        if (err != Z_OK) {
            std::cerr << "inflateInit failed: " << err << "\n";
            abort();
        }

        err = inflate(&stream, Z_FINISH);

        if( stream.msg != NULL ) {
            std::cerr << stream.msg << "\n";
        }


        uLongf quux_size = quux.size();
        err = uncompress( quux.data(), &quux_size, IDAT.data() + 8, IDAT.size() - 8 );
        if( err != Z_OK ) {
            std::cerr << "uncompress="
                      << err
                      << "\n";
        }
        if( quux_size != ((3*WIDTH+1)*HEIGHT) ) {
            std::cerr << "uncompress_size="  << quux_size
                      << ", should be=" << ((3*WIDTH+1)*HEIGHT) << "\n";
        }

    }
#endif

    std::cerr << "filter+LZenc=" << TimeStamp::delta( T0, T1 )
              << ", adler32+huffenc=" << TimeStamp::delta( T1, T2 )
              << ", crc32=" << TimeStamp::delta( T4, T5 )
              << ", io=" << TimeStamp::delta( T5, T6 )
              << ", total=" << TimeStamp::delta( T0, T6 );
}



void
writeIDAT4( ThreadPool *thread_pool, std::ofstream& file, const std::vector<char>& img, const std::vector<unsigned long>& crc_table, int WIDTH, int HEIGHT  )
{
    int T = (thread_pool->workers()+1);


    TimeStamp T0;
    unsigned int adler;
    unsigned int filtered_size = (3*WIDTH+1)*HEIGHT;
    unsigned char* filtered = (unsigned char*)malloc( sizeof(unsigned char)*filtered_size );
    unsigned int* codestream = (unsigned int*)malloc(sizeof(unsigned int)*filtered_size );

    filterScanlines( filtered, (unsigned char*)(img.data()), WIDTH, HEIGHT );


    TimeStamp T1;
    //adler = computeAdler32( filtered.data(), filtered.size() );
    //adler = computeAdler32Blocked( filtered.data(), filtered.size() );
    adler = computeAdler32SSE( filtered, filtered_size );

    TimeStamp T2;
    

    // --- Find string duplicates and create code stream -----------------------
    unsigned int M = encodeLZ( codestream, filtered, filtered_size );
    TimeStamp T3;

    // --- Encode using fixed Huffman codes ------------------------------------
    std::vector<unsigned char> IDAT(8);
    // IDAT chunk header
    IDAT[4] = 'I';
    IDAT[5] = 'D';
    IDAT[6] = 'A';
    IDAT[7] = 'T';

    // --- create deflate chunk ------------------------------------------------

    unsigned int* code_stream_p[1] = { codestream };
    unsigned int  code_stream_N[1] = { M /*codestream.size()*/ };
    encodeHuffman( IDAT, code_stream_p, code_stream_N, 1 );

    TimeStamp T4;

    
    IDAT.push_back( ((adler)>>24)&0xffu ); // Adler32
    IDAT.push_back( ((adler)>>16)&0xffu );
    IDAT.push_back( ((adler)>> 8)&0xffu );
    IDAT.push_back( ((adler)>> 0)&0xffu );

    // --- end deflate chunk --------------------------------------------------

    // Update PNG chunk content size for IDAT
    int dat_size = IDAT.size()-8u;
    IDAT[0] = ((dat_size)>>24)&0xffu;
    IDAT[1] = ((dat_size)>>16)&0xffu;
    IDAT[2] = ((dat_size)>>8)&0xffu;
    IDAT[3] = ((dat_size)>>0)&0xffu;

    unsigned long crc = CRC( crc_table, IDAT.data()+4, dat_size+4 );
    IDAT.resize( IDAT.size()+4u );  // make room for CRC
    IDAT[dat_size+8]  = ((crc)>>24)&0xffu;
    IDAT[dat_size+9]  = ((crc)>>16)&0xffu;
    IDAT[dat_size+10] = ((crc)>>8)&0xffu;
    IDAT[dat_size+11] = ((crc)>>0)&0xffu;

    TimeStamp T5;

    file.write( reinterpret_cast<char*>( IDAT.data() ), IDAT.size() );

    TimeStamp T6;

#if 1
    if( 1) {
        std::vector<unsigned char> quux(10*1024*1024);

        z_stream stream;
        int err;
    
        stream.next_in = (z_const Bytef *)IDAT.data()+8;
        stream.avail_in = (uInt)IDAT.size()-8;
        stream.next_out = quux.data();
        stream.avail_out = quux.size();
        stream.zalloc = (alloc_func)0;
        stream.zfree = (free_func)0;

        err = inflateInit(&stream);
        if (err != Z_OK) {
            std::cerr << "inflateInit failed: " << err << "\n";
            abort();
        }
       
        err = inflate(&stream, Z_FINISH);

        if( stream.msg != NULL ) {
            std::cerr << stream.msg << "\n";
        }
        

        uLongf quux_size = quux.size();
        err = uncompress( quux.data(), &quux_size, IDAT.data() + 8, IDAT.size() - 8 );
        if( err != Z_OK ) {
            std::cerr << "uncompress="
                      << err
                      << "\n";
        }
        if( quux_size != ((3*WIDTH+1)*HEIGHT) ) {
            std::cerr << "uncompress_size="  << quux_size
                      << ", should be=" << ((3*WIDTH+1)*HEIGHT) << "\n";
        }
        
    }
#endif

    std::cerr << "filter=" << TimeStamp::delta( T0, T1 )
              << ", adler32=" << TimeStamp::delta( T1, T2 )
              << ", LZenc=" << TimeStamp::delta( T2, T3 )
              << ", huffenc=" << TimeStamp::delta( T3, T4 )
              << ", crc32=" << TimeStamp::delta( T4, T5 )
              << ", io=" << TimeStamp::delta( T5, T6 )
              << ", total=" << TimeStamp::delta( T0, T6 );

}




void
writeIEND( std::ofstream& file, const std::vector<unsigned long>& crc_table  )
{
    unsigned char IEND[12] = {
        0, 0, 0, 0,         // payload size
        'I', 'E', 'N', 'D', // chunk id
        174, 66, 96, 130    // chunk crc
    };
    file.write( reinterpret_cast<char*>( IEND ), 12 );
 }




int
homebrew_png2( const std::vector<char> &rgb,
              const int w,
              const int h )
{
    std::ofstream png( "homebrew2.png" );
    writeSignature( png );
    writeIHDR( png, crc_table, w, h );
    writeIDAT2( png, rgb, crc_table, w, h );
    writeIEND( png, crc_table );

    int bytes = png.tellp();
    png.close();
    
    return bytes;
}

int
homebrew_png3( const std::vector<char> &rgb,
              const int w,
              const int h )
{
    std::ofstream png( "homebrew3.png" );
    writeSignature( png );
    writeIHDR( png, crc_table, w, h );
    writeIDAT3( png, rgb, crc_table, w, h );
    writeIEND( png, crc_table );

    int bytes = png.tellp();
    png.close();
    
    return bytes;
}

int
homebrew_png4(ThreadPool *thread_pool, const std::vector<char> &rgb,
              const int w,
              const int h )
{
    std::ofstream png( "homebrew4.png" );
    writeSignature( png );
    writeIHDR( png, crc_table, w, h );
    writeIDAT4( thread_pool, png, rgb, crc_table, w, h );
    writeIEND( png, crc_table );

    int bytes = png.tellp();
    png.close();

    return bytes;
}
int
homebrew_png4_mc(ThreadPool *thread_pool, const std::vector<char> &rgb,
              const int w,
              const int h )
{
    std::ofstream png( "homebrew4.png" );
    writeSignature( png );
    writeIHDR( png, crc_table, w, h );
    writeIDAT4MC( thread_pool, png, rgb, crc_table, w, h );
    writeIEND( png, crc_table );

    int bytes = png.tellp();
    png.close();

    return bytes;
}
