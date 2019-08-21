#include <cppx-core/all.hpp>        // <url: https://github.com/alf-p-steinbach/cppx-core>
#include <winapi-header-wrappers/windows-h.hpp>     // A <windows.h> wrapper with UNICODE.
#include <winapi-header-wrappers/shellapi-h.hpp>    // CommandLineToArgvW

namespace win_util {
    $use_std( exchange );
    $use_cppx( hopefully );
 
    using Mutable_wide_c_str    = wchar_t*;
    using Wide_c_str            = const wchar_t*;

    class Command_line_args
    {
        Mutable_wide_c_str*     m_parts;
        int                     m_n_parts;
        
        Command_line_args( const Command_line_args& ) = delete;
        auto operator=( const Command_line_args& ) -> Command_line_args& = delete;
        
    public:
        auto count() const -> int                           { return m_n_parts - 1; }
        auto operator[]( const int i ) const -> Wide_c_str  { return m_parts[i + 1]; }
        auto invocation() const -> Wide_c_str               { return m_parts[0]; }
            
        Command_line_args():
            m_parts( CommandLineToArgvW( GetCommandLine(), &m_n_parts ) )
        {
            hopefully( m_parts != nullptr )
                or $fail( "CommandLineToArgvW failed" );
        }
        
        Command_line_args( Command_line_args&& other ):
            m_parts( exchange( other.m_parts, nullptr ) ),
            m_n_parts( exchange( other.m_n_parts, 0 ) )
        {}

        ~Command_line_args()
        {
            if( m_parts != nullptr ) {
                LocalFree(  m_parts );
            }
        }
    };
}  // namespace win_util

namespace app {
    $use_std(
        cout, clog, endl, invoke, runtime_error, string, vector
        );
    $use_cppx(
        noreturn, hopefully, fail_, Is_zero,
        Byte, fs_util::C_file, Size, Index, C_str,
        fs_util::read, fs_util::read_, fs_util::read_sequence, fs_util::read_sequence_,
        fs_util::peek_,
        bits_per_, is_in, to_hex, up_to
        );
    namespace fs = std::filesystem;
    using namespace cppx::basic_string_building;        // operator<<, operator""s

    // A class to serve simple failure messages to the user, via exceptions. These
    // exceptions are thrown without origin info, and are presented as just strings.
    // Don't do this in any commercial code.
    class Ui_exception:
        public runtime_error
    { using runtime_error::runtime_error; };
    
    using Uix = Ui_exception;

    struct Pe32_types
    {
        using Optional_header = IMAGE_OPTIONAL_HEADER32;
        static constexpr int address_width = 32;
    };
    
    struct Pe64_types
    {
        using Optional_header = IMAGE_OPTIONAL_HEADER64;
        static constexpr int address_width = 64;
    };

    // When this function is called the file position is at start of the optional header.
    template< class Pe_types >
    void list_exports(
        const string&               u8_path,
        const C_file&               f,
        const IMAGE_FILE_HEADER&    pe_header
        )
    {
        using Optional_header = typename Pe_types::Optional_header;
        const auto pe_header_opt = read_<Optional_header>( f );

        hopefully( IMAGE_DIRECTORY_ENTRY_EXPORT < pe_header_opt.NumberOfRvaAndSizes )
            or fail_<Uix>( ""s << "No exports found in '" << u8_path << "'." );
            
        const auto section_headers = invoke( [&]()
            -> vector<IMAGE_SECTION_HEADER>
        {
            vector<IMAGE_SECTION_HEADER> headers;
            for( int _: up_to( pe_header.NumberOfSections ) ) {
                (void) _;  headers.push_back( read_<IMAGE_SECTION_HEADER>( f ) );
            }
            return headers;
        } );
        
        const auto& dir_info = pe_header_opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

        hopefully( dir_info.Size >= sizeof( IMAGE_EXPORT_DIRECTORY ) )
            or fail_<Uix>( "Ungood file: claimed size of export dir header is too small." );
            
        auto& section = invoke( [&]() -> const IMAGE_SECTION_HEADER&
        {
            const auto dir_addr         = dir_info.VirtualAddress;
            const auto beyond_dir_addr  = dir_addr + dir_info.Size;
        
            for( const auto& s: section_headers ) {
                const auto s_addr           = s.VirtualAddress;
                const auto beyond_s_addr    = s_addr + s.SizeOfRawData;
                
                if( s_addr <= dir_addr and beyond_dir_addr <= beyond_s_addr ) {
                    return s;
                }
            }
            fail_<Uix>( "Ungood file: no section (fully) contains the export table." );
            noreturn();
        } );

        hopefully( section.SizeOfRawData > 0 )
            or fail_<Uix>( "Ungood file: section with export table, is of length zero." );

        const INT32 addr_to_pos     = section.PointerToRawData - section.VirtualAddress;
        const DWORD dir_position    = dir_info.VirtualAddress + addr_to_pos;
    
        fseek( f, dir_position, SEEK_SET ) >> Is_zero()
            or fail_<Uix>( "Ungood file: a seek to the exports table section failed." );
        const auto  dir             = read_<IMAGE_EXPORT_DIRECTORY>( f );
        const INT32 ordinal_base    = dir.Base;

        fseek( f, dir.Name + addr_to_pos, SEEK_SET ) >> Is_zero()
            or fail_<Uix>( "Ungood file: a seek to the module name failed." );
        const string module_name = read_c_string( f );
            
        cout    << Pe_types::address_width << "-bit module"
                << " (as viewed from " << bits_per_<void*> << "-bit code)"
                << ", module name \"" << module_name << "\""
                << "." << endl;

        if( dir.NumberOfFunctions == 0 ) {
            cout << "No functions are exported";
        } else if( dir.NumberOfFunctions == 1 ) {
            cout << "1 function is exported, at ordinal " << ordinal_base;
        } else if( dir.NumberOfFunctions > 1 ) {
            const INT32 last_ordinal = ordinal_base + dir.NumberOfFunctions - 1;
            cout    << dir.NumberOfFunctions << " functions are exported"
                    << ", at ordinals " << ordinal_base << "..." << last_ordinal;
        }
        cout << "." << endl;
    
        if( dir.NumberOfFunctions == 0 ) {
            return;
        }
        
        fseek( f, dir.AddressOfNames + addr_to_pos, SEEK_SET ) >> Is_zero()
            or fail_<Uix>( "Ungood file: a seek to the name addresses table failed." );
        const vector<DWORD> name_positions = read_sequence_<DWORD>( f, dir.NumberOfNames );

        vector<string> export_names;
        export_names.reserve( name_positions.size() );
        for( const DWORD name_addr: name_positions ) {
            fseek( f, name_addr + addr_to_pos, SEEK_SET ) >> Is_zero()
                or fail_<Uix>( "Ungood file: a seek to the an export name failed." );
            export_names.push_back( read_c_string( f ) );
        }
        
        fseek( f, dir.AddressOfNameOrdinals + addr_to_pos, SEEK_SET ) >> Is_zero()
            or fail_<Uix>( "Ungood file: a seek to the ordinals table failed." );
        const vector<WORD> ordinals = read_sequence_<WORD>( f, dir.NumberOfNames );

        cout << string( 72, '-' ) << endl;
        for( const int i: up_to( dir.NumberOfNames ) ) {
            cout << export_names[i] << " @" << ordinals[i] + ordinal_base << endl;
        }
    }

    template< class Pe_types >
    void display_info(
        const string&               u8_path,
        const C_file&               f,
        const IMAGE_FILE_HEADER&    pe_header
        )
    {
        try {
            list_exports<Pe_types>( u8_path, f, pe_header );
        } catch( ... ) {
            cout << Pe_types::address_width << "-bit module." << endl;
            throw;
        }
    }

    void run()
    {
        const auto args             = win_util::Command_line_args();
        hopefully( args.count() == 1 )
            or fail_<Uix>( "Specify one argument: the DLL filename or path." );

        const fs::path dll_path     = args[0];
        const string u8_path = cppx::fs_util::utf8_from( dll_path );

        const auto f = C_file( tag::Read(), dll_path );

        const auto dos_header       = read_<IMAGE_DOS_HEADER >( f );
        hopefully( dos_header.e_magic == IMAGE_DOS_SIGNATURE )  //0x5A4D, 'MZ' multichar.
            or fail_<Uix>( ""s << "No MZ magic number at start of '" << u8_path << "'." );
            
        fseek( f, dos_header.e_lfanew, SEEK_SET ) >> Is_zero()
            or fail_<Uix>( "fseek to PE header failed" );
            
        const auto pe_signature     = read_<DWORD>( f );
        hopefully( pe_signature == IMAGE_NT_SIGNATURE  )        //0x4550, 'PE' multichar.
            or fail_<Uix>( ""s << "No PE magic number in PE header of '" << u8_path << "'." );

        const auto pe_header        = read_<IMAGE_FILE_HEADER>( f );
        const auto image_kind_spec  = peek_<WORD>( f );

        switch( image_kind_spec ) {
            case IMAGE_NT_OPTIONAL_HDR32_MAGIC: {               // 0x10B
                display_info<Pe32_types>( u8_path, f, pe_header );
                break;
            }
            case IMAGE_NT_OPTIONAL_HDR64_MAGIC: {               // 0x20B
                display_info<Pe64_types>( u8_path, f, pe_header );
                break;
            }
            default: {      // E.g. 0x107 a.k.a. IMAGE_ROM_OPTIONAL_HDR_MAGIC
                fail_<Uix>( "Not a PE32 (32-bit) or PE32+ (64-bit) file." );
            }
        };
    }
}  // namespace app

auto main() -> int
{
    $use_std( exception, cerr, endl, clog, ios_base );
    $use_cppx( monospaced_bullet_block, description_lines_from );

    #ifdef NDEBUG
        clog.setstate( ios_base::failbit );     // Suppress trace output.
    #endif
    try {
        app::run();
        return EXIT_SUCCESS;
    } catch( const app::Ui_exception& x ) {
        cerr << "!" << x.what() << endl;
    } catch( const exception& x ) {
        cerr << monospaced_bullet_block( description_lines_from( x ) ) << endl;
    }
    return EXIT_FAILURE;
}
