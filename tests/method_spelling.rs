use oreslang_format::{format_source, is_formatted};

#[test]
fn actor_methods_lose_legacy_fnc_but_preserve_explicit_method_and_static_fnc() {
    let input = r#"define actor ConnectionActor as
final rooms: Map<RoomId, ActorGroup>

suspend fnc join_room(
group: ActorGroup,
permit: ActorGroupJoinCapability
) {
group.join(permit)
}

fnc leave_room(group: ActorGroup) {
group.leave()
}

method explicit_room(group: ActorGroup) {
group.leave()
}

static fnc room_count(group: ActorGroup) {
return 1;
}
end

fnc top_level(group: ActorGroup) {
return;
}
"#;
    let formatted = format_source(input).unwrap();
    assert!(formatted.contains("suspend join_room("), "{formatted}");
    assert!(formatted.contains("  group: ActorGroup,"), "{formatted}");
    assert!(formatted.contains("  permit: ActorGroupJoinCapability"), "{formatted}");
    assert!(formatted.contains("leave_room(group: ActorGroup)"), "{formatted}");
    assert!(formatted.contains("method explicit_room(group: ActorGroup)"), "{formatted}");
    assert!(formatted.contains("static fnc room_count(group: ActorGroup)"), "{formatted}");
    assert!(formatted.contains("fnc top_level(ActorGroup group)"), "{formatted}");
    assert!(!formatted.contains("fnc join_room"), "{formatted}");
    assert!(!formatted.contains("fnc leave_room"), "{formatted}");
    assert!(is_formatted(&formatted).unwrap());
}

#[test]
fn class_and_braced_actor_members_canonicalize_but_method_bodies_are_not_rewritten() {
    let input = r#"define class C as
fnc value(): int {
val marker = "fnc not_a_declaration()";
return 1;
}
method explicit(): int {
return 2;
}
static fnc seed(): int {
return 3;
}
end

shared actor Relay {
pub fnc on_message(): int {
return 1;
}
}
"#;
    let formatted = format_source(input).unwrap();
    assert!(formatted.contains("  value() -> int {"), "{formatted}");
    assert!(formatted.contains("  method explicit() -> int {"), "{formatted}");
    assert!(formatted.contains("  static fnc seed() -> int {"), "{formatted}");
    assert!(formatted.contains("  pub on_message() -> int {"), "{formatted}");
    assert!(formatted.contains("\"fnc not_a_declaration()\""), "{formatted}");
    assert!(is_formatted(&formatted).unwrap());
}
