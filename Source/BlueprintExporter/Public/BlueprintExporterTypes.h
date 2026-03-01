#pragma once

#include "CoreMinimal.h"

struct FExportedPin
{
	FString Name;
	FString Direction;      // "Input" / "Output"
	FString Category;       // exec, bool, int, real, object, struct, byte, name, string, etc.
	FString SubType;        // Specific subtype: Character, Vector, E_AI_State, float, double
	FString ContainerType;  // Array, Set, Map, or empty
	FString DefaultValue;
	bool bIsHidden = false;

	// Connection info: (TargetNodeName, TargetPinDisplayName)
	TArray<TPair<FString, FString>> LinkedTo;
};

struct FExportedVariable
{
	FString Name;
	FString Type;           // "float", "E_AI_State", "Actor", "Vector"
	FString ContainerType;  // "Array", "Set", "Map", or empty
	FString DefaultValue;
	TArray<FString> Flags;  // "EditAnywhere", "BlueprintReadWrite", etc.
};

struct FExportedGraph; // Forward declaration for SubGraph

struct FExportedNode
{
	FString NodeName;       // e.g. "K2Node_Event_0"
	FString NodeClass;      // e.g. "K2Node_Event"
	FString GraphName;

	// Ordered properties: Event, Function, Variable, CastTo, Override, Macro, etc.
	// Using TArray instead of TMap to preserve insertion order for display.
	TArray<TPair<FString, FString>> Properties;
	TArray<FExportedPin> Pins;

	TSharedPtr<FExportedGraph> SubGraph; // Expanded subgraph for Composite (collapsed) nodes
};

struct FExportedGraph
{
	FString GraphName;      // "EventGraph", "SetCurrentState", etc.
	FString GraphType;      // "EventGraph", "Function", "Macro"
	TArray<FExportedNode> Nodes;
};

struct FExportedBlueprint
{
	FString BlueprintName;
	FString ParentClass;
	TArray<FExportedVariable> Variables;
	TArray<FExportedGraph> Graphs;
};
