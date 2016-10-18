package pack is

    function add4(x : in integer) return integer;

end package;

package body pack is

    function add4(x : in integer) return integer is
    begin
        return x + 4;
    end function;

end package body;

-------------------------------------------------------------------------------

entity ffold is
end entity;

use work.pack.all;

architecture a of ffold is

    function add1(x : in integer) return integer is
    begin
        return x + 1;
    end function;

    function log2(x : in integer) return integer is
        variable r : integer := 0;
        variable c : integer := 1;
    begin
        --while true loop
        --end loop;
        if x <= 1 then
            r := 1;
        else
            while c < x loop
                r := r + 1;
                c := c * 2;
            end loop;
        end if;
        return r;
    end function;

    function case1(x : in integer) return integer is
    begin
        case x is
            when 1 =>
                return 2;
            when 2 =>
                return 3;
            when others =>
                return 5;
        end case;
    end function;

    function adddef(x, y : in integer := 5) return integer is
    begin
        return x + y;
    end function;

    function chain1(x : string) return boolean is
        variable r : boolean := false;
    begin
        if x = "hello" then
            r := true;
        end if;
        return r;
    end function;

    function chain2(x, y : string) return boolean is
        variable r : boolean := false;
    begin
        if chain1(x) or chain1(y) then
            r := true;
        end if;
        return r;
    end function;

    function flip(x : bit_vector(3 downto 0)) return bit_vector is
        variable r : bit_vector(3 downto 0);
    begin
        r(0) := x(3);
        r(1) := x(2);
        r(2) := x(1);
        r(3) := x(0);
        return r;
    end function;

    type real_vector is array (natural range <>) of real;

    function lookup(index : integer) return real is
        constant table : real_vector := (
            0.62, 61.62, 71.7, 17.25, 26.15, 651.6, 0.45, 5.761 );
    begin
        return table(index);
    end function;

    function get_bitvec(x, y : integer) return bit_vector is
        variable r : bit_vector(x to y) := "00";
    begin
        return r;
    end function;
begin

    b1: block is
        signal s1  : integer := add1(5);
        signal s2  : integer := add4(1);
        signal s3  : integer := log2(11);
        signal s4  : integer := log2(integer(real'(5.5)));
        signal s5  : integer := case1(1);
        signal s6  : integer := case1(7);
        signal s7  : integer := adddef;
        signal s8  : boolean := chain2("foo", "hello");
        signal s9  : boolean := flip("1010") = "0101";
        signal s10 : boolean := flip("1010") = "0111";
        signal s11 : real := lookup(0);  -- 0.62;
        signal s12 : real := lookup(2);  -- 71.7;
        signal s13 : boolean := get_bitvec(1, 2) = "00";
    begin
    end block;

end architecture;
