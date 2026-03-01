#pragma once

#include "CoreMinimal.h"
#include "BlueprintExporterTypes.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

class FBlueprintGraphExtractor
{
public:
	FExportedBlueprint Extract(UBlueprint* Blueprint);

	FExportedGraph ExtractSelectedNodes(
		const TSet<UEdGraphNode*>& SelectedNodes,
		const UEdGraph* OwningGraph);

private:
	FExportedGraph ExtractGraph(UEdGraph* Graph, const FString& GraphType);
	FExportedNode ExtractNode(UEdGraphNode* Node, const FString& GraphName);
	void ExtractNodeProperties(UEdGraphNode* Node, FExportedNode& OutNode);
	FExportedPin ExtractPin(UEdGraphPin* Pin);

	static FString ResolveVariableType(const FEdGraphPinType& PinType);
};
