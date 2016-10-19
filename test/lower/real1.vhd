entity real1 is
end entity;

architecture test of real1 is

    function get_neg(x : real) return real is
        variable r : real;
    begin
        r := -x;
        return r;
    end function;

begin

end architecture;
