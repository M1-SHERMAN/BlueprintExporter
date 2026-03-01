#pragma once

#include "CoreMinimal.h"
#include "BlueprintExporterTypes.h"

class FBlueprintTextFormatter
{
public:
	FString Format(const FExportedBlueprint& Blueprint);
	FString FormatSelectedNodes(const FExportedGraph& Graph, const FString& BlueprintName);
	FString FormatGraphOnly(const FExportedGraph& Graph);
	FString FormatSummary(const FExportedBlueprint& Blueprint);

private:
	FString FormatVariables(const TArray<FExportedVariable>& Variables);
	FString FormatGraph(const FExportedGraph& Graph);
	FString FormatNode(const FExportedNode& Node);
	FString FormatPin(const FExportedPin& Pin);

	FString GetReadableType(const FString& NodeClass);

	TArray<FExportedNode> TopologicalSort(const TArray<FExportedNode>& Nodes);
	TArray<FExportedPin> GetMeaningfulPins(const FExportedNode& Node);

	bool IsTrivialDefault(const FString& Value);

	TPair<FString, FString> ResolveRerouteChain(const FString& NodeName, const FString& OriginalPinName, const TMap<FString, const FExportedNode*>& NodeMap);

	FString FormatExecutionFlow(const TArray<FExportedNode>& Nodes, const TMap<FString, const FExportedNode*>& NodeMap);
	FString FormatDataFlow(const TArray<FExportedNode>& Nodes, const TMap<FString, const FExportedNode*>& NodeMap);

	TSet<FString> SelectionContext; // Empty = full export; non-empty = selection mode
};
