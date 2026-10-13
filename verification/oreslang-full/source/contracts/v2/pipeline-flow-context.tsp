// Companion TypeSpec-shaped host contract. JSON serialization is for
// diagnostics/verification ONLY; this must never be treated as a guest
// MessagePack v2 envelope or as an untrusted actor route capability.
namespace Oreslang.ActorPipeline;

scalar DecimalU64 extends string;

model ActorEndpointIdentity {
  actorId: DecimalU64;
  actorGeneration: DecimalU64;
  schedulerDomain: DecimalU64;
}

model HostPipelineFlowContextV1 {
  version: 1;
  pipelineId: DecimalU64;
  routeGeneration: DecimalU64;
  flowId: DecimalU64;
  correlationId: DecimalU64;
  messageId: DecimalU64;
  parentMessageId: DecimalU64;
  stageIndex: int32;
  direction: "root" | "forward" | "backward";
  sender: ActorEndpointIdentity;
  destination: ActorEndpointIdentity;
}
