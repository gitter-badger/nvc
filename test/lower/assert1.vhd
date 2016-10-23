entity sub is
    generic ( X : integer );
end entity;

architecture test of sub is
begin
    assert X > 5 report "X is " & integer'image(x);
end architecture;

-------------------------------------------------------------------------------

entity assert1 is
end entity;

architecture test of assert1 is
begin
    sub_i: entity work.sub generic map ( 6 );
end architecture;
