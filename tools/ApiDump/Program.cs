using System.Reflection;

// Dumps the public API surface of the Facepunch.Steamworks assembly that the project
// references, so the game code can be written against exactly what ships in the package.

var asm = typeof(Steamworks.SteamClient).Assembly;
var an = asm.GetName();
Console.WriteLine($"ASSEMBLY : {an.Name} {an.Version}");
Console.WriteLine($"LOCATION : {asm.Location}");
Console.WriteLine();

var types = asm.GetExportedTypes();

string Pretty(Type t)
{
    if (t == null) return "<null>";
    if (!t.IsGenericType) return t.Name;
    return t.Name.Split('`')[0] + "<" + string.Join(", ", t.GetGenericArguments().Select(Pretty)) + ">";
}

string Member(MemberInfo m)
{
    switch (m)
    {
        case MethodInfo mi:
            var ps = string.Join(", ", mi.GetParameters().Select(p =>
                Pretty(p.ParameterType) + " " + p.Name + (p.HasDefaultValue ? " = " + (p.DefaultValue ?? "null") : "")));
            return $"{Pretty(mi.ReturnType)} {mi.Name}({ps})";
        case PropertyInfo pi:
            return $"{Pretty(pi.PropertyType)} {pi.Name} {{ {(pi.CanRead ? "get; " : "")}{(pi.CanWrite ? "set; " : "")}}}";
        case FieldInfo fi:
            return $"{Pretty(fi.FieldType)} {fi.Name}";
        default:
            return m.Name;
    }
}

var wanted = new[]
{
    "SteamClient", "SteamFriends", "SteamMatchmaking", "SteamNetworking", "Dispatch",
    "Lobby", "LobbyQuery", "P2Packet", "Friend", "P2PSend", "LobbyType", "Result", "RoomEnter",
};

foreach (var name in wanted)
{
    var t = types.FirstOrDefault(x => x.Name == name);
    if (t == null)
    {
        Console.WriteLine($"### {name}: *** NOT FOUND ***");
        Console.WriteLine();
        continue;
    }

    Console.WriteLine($"### {t.FullName}  (enum={t.IsEnum})");

    if (t.IsEnum)
    {
        foreach (var v in Enum.GetNames(t))
            Console.WriteLine($"    {v} = {Convert.ToInt64(Enum.Parse(t, v))}");
        Console.WriteLine();
        continue;
    }

    foreach (var m in t.GetMembers(BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static | BindingFlags.DeclaredOnly)
                        .Where(m => m.MemberType is MemberTypes.Method or MemberTypes.Property or MemberTypes.Field or MemberTypes.Event)
                        .OrderBy(m => m.Name))
    {
        if (m is MethodInfo mi && mi.IsSpecialName) continue;
        Console.WriteLine($"    {m.MemberType,-8} {Member(m)}");
    }
    Console.WriteLine();
}

Console.WriteLine("### Types matching Networking/Lobby/P2P:");
foreach (var t in types.Where(t => t.Name.Contains("Networking") || t.Name.Contains("Lobby") || t.Name.Contains("P2P")).OrderBy(t => t.Name))
    Console.WriteLine($"    {t.FullName}");

Console.WriteLine();
Console.WriteLine("DUMP-COMPLETE");
